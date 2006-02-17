/******************************************************************************\
 * Copyright (c) 2004-2006
 *
 * Author(s):
 *	Volker Fischer
 *
 ******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later 
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more 
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
\******************************************************************************/

#include "client.h"


/* Implementation *************************************************************/
bool CClient::SetServerAddr(QString strNAddr)
{
	QHostAddress InetAddr;

	if (InetAddr.setAddress(strNAddr))
	{
		/* The server port is fixed and always the same */
		Channel.SetAddress(CHostAddress(InetAddr, LLCON_PORT_NUMBER));

		return true;
	}
	else
	{
		return false; /* invalid address */
	}
}

void CClient::Init()
{
	/* set block sizes (in samples) */
	iBlockSizeSam = MIN_BLOCK_SIZE_SAMPLES;
	iSndCrdBlockSizeSam = MIN_SND_CRD_BLOCK_SIZE_SAMPLES;

	vecsAudioSndCrd.Init(iSndCrdBlockSizeSam * 2); /* stereo */
	vecdAudioSndCrdL.Init(iSndCrdBlockSizeSam);
	vecdAudioSndCrdR.Init(iSndCrdBlockSizeSam);

	vecdAudioL.Init(iBlockSizeSam);
	vecdAudioR.Init(iBlockSizeSam);

	Sound.InitRecording(iSndCrdBlockSizeSam * 2 /* stereo */);
	Sound.InitPlayback(iSndCrdBlockSizeSam * 2 /* stereo */);

	/* resample objects are always initialized with the input block size */
	/* record */
	ResampleObjDownL.Init(iSndCrdBlockSizeSam,
		(double) SAMPLE_RATE / SND_CRD_SAMPLE_RATE);
	ResampleObjDownR.Init(iSndCrdBlockSizeSam,
		(double) SAMPLE_RATE / SND_CRD_SAMPLE_RATE);

	/* playback */
	ResampleObjUpL.Init(iBlockSizeSam,
		(double) SND_CRD_SAMPLE_RATE / SAMPLE_RATE);
	ResampleObjUpR.Init(iBlockSizeSam,
		(double) SND_CRD_SAMPLE_RATE / SAMPLE_RATE);

	/* init network buffers */
	vecsNetwork.Init(iBlockSizeSam);
	vecdNetwData.Init(iBlockSizeSam);

	/* init moving average buffer for response time evaluation */
	RespTimeMoAvBuf.Init(LEN_MOV_AV_RESPONSE);

	/* init time for response time evaluation */
	TimeLastBlock = QTime::currentTime();

	AudioReverb.Clear();
}

void CClient::run()
{
	int i, iInCnt;

	/* Set thread priority (The working thread should have a higher
	   priority than the GUI) */
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    /* set the process to realtime privs */
	struct sched_param schp;
    memset(&schp, 0, sizeof(schp));
    schp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    sched_setscheduler(0, SCHED_FIFO, &schp);
#endif

	/* init object */
	Init();


	/* runtime phase --------------------------------------------------------- */
	bRun = true;

	/* main loop of working thread */
	while (bRun)
	{
		/* get audio from sound card (blocking function) */
		if (Sound.Read(vecsAudioSndCrd))
			PostWinMessage(MS_SOUND_IN, MUL_COL_LED_RED);
		else
			PostWinMessage(MS_SOUND_IN, MUL_COL_LED_GREEN);

		/* copy data from one stereo buffer in two separate buffers */
		iInCnt = 0;
		for (i = 0; i < iSndCrdBlockSizeSam; i++)
		{
			vecdAudioSndCrdL[i] = (double) vecsAudioSndCrd[iInCnt++];
			vecdAudioSndCrdR[i] = (double) vecsAudioSndCrd[iInCnt++];
		}

		/* resample data for each channel seaparately */
		ResampleObjDownL.Resample(vecdAudioSndCrdL, vecdAudioL);
		ResampleObjDownR.Resample(vecdAudioSndCrdR, vecdAudioR);

		/* update signal level meters */
		SignalLevelMeterL.Update(vecdAudioL);
		SignalLevelMeterR.Update(vecdAudioR);

		/* add reverberation effect if activated */
		if (iReverbLevel != 0)
		{
			/* first attenuation amplification factor */
			const double dRevLev = (double) iReverbLevel / AUD_REVERB_MAX / 2;

			if (bReverbOnLeftChan)
			{
				for (i = 0; i < iBlockSizeSam; i++)
				{
					/* left channel */
					vecdAudioL[i] +=
						dRevLev * AudioReverb.ProcessSample(vecdAudioL[i]);
				}
			}
			else
			{
				for (i = 0; i < iBlockSizeSam; i++)
				{
					/* right channel */
					vecdAudioR[i] +=
						dRevLev * AudioReverb.ProcessSample(vecdAudioR[i]);
				}
			}
		}

		/* mix both signals depending on the fading setting */
		const int iMiddleOfFader = AUD_FADER_IN_MAX / 2;
		const double dAttFact =
			(double) (iMiddleOfFader - abs(iMiddleOfFader - iAudioInFader)) /
			iMiddleOfFader;
		for (i = 0; i < iBlockSizeSam; i++)
		{
			double dMixedSignal;

			if (iAudioInFader > iMiddleOfFader)
				dMixedSignal = vecdAudioL[i] + dAttFact * vecdAudioR[i];
			else
				dMixedSignal = vecdAudioR[i] + dAttFact * vecdAudioL[i];

			vecsNetwork[i] = Double2Short(dMixedSignal);
		}

		/* send it through the network */
		Socket.SendPacket(Channel.PrepSendPacket(vecsNetwork),
			Channel.GetAddress(), Channel.GetTimeStampIdx());

		/* receive a new block */
		if (Channel.GetData(vecdNetwData))
			PostWinMessage(MS_JIT_BUF_GET, MUL_COL_LED_GREEN);
		else
			PostWinMessage(MS_JIT_BUF_GET, MUL_COL_LED_RED);

#ifdef _DEBUG_
#if 0
#if 0
/* Determine network delay. We can do this very simple if only this client is
   connected to the server. In this case, exactly the same audio material is
   coming back and we can simply compare the samples */
/* store send data instatic buffer (may delay is 100 ms) */
const int iMaxDelaySamples = (int) ((float)       0.3     /*0.1*/ * SAMPLE_RATE);
static CVector<short> vecsOutBuf(iMaxDelaySamples);

/* update buffer */
const int iBufDiff = iMaxDelaySamples - iBlockSizeSam;
for (i = 0; i < iBufDiff; i++)
	vecsOutBuf[i + iBlockSizeSam] = vecsOutBuf[i];
for (i = 0; i < iBlockSizeSam; i++)
	vecsOutBuf[i] = vecsNetwork[i];

/* now search for equal samples */
int iDelaySamples = 0;
for (i = 0; i < iMaxDelaySamples - 1; i++)
{
	/* compare two successive samples */
	if ((vecsOutBuf[i] == (short) vecdNetwData[0]) &&
		(vecsOutBuf[i + 1] == (short) vecdNetwData[1]))
	{
		iDelaySamples = i;
	}
}

static FILE* pFileDelay = fopen("delay.dat", "w");
fprintf(pFileDelay, "%d\n", iDelaySamples);
fflush(pFileDelay);
#else
/* just store both, input and output, streams */
// fid=fopen('v.dat','r');x=fread(fid,'int16');fclose(fid);
static FILE* pFileDelay = fopen("v.dat", "wb");
short sData[2];
for (i = 0; i < iBlockSizeSam; i++)
{
	sData[0] = vecsNetwork[i];
	sData[1] = (short) vecdNetwData[i];
	fwrite(&sData, size_t(2), size_t(2), pFileDelay);
}
fflush(pFileDelay);
#endif
#endif
#endif


/*
// fid=fopen('v.dat','r');x=fread(fid,'int16');fclose(fid);
static FILE* pFileDelay = fopen("v.dat", "wb");
short sData[2];
for (i = 0; i < iBlockSizeSam; i++)
{
	sData[0] = (short) vecdNetwData[i];
	fwrite(&sData, size_t(2), size_t(1), pFileDelay);
}
fflush(pFileDelay);
*/


		/* check if channel is connected */
		if (Channel.IsConnected())
		{
			/* write mono input signal in both sound-card channels */
			for (i = 0; i < iBlockSizeSam; i++)
				vecdAudioL[i] = vecdAudioR[i] = vecdNetwData[i];
		}
		else
		{
			/* if not connected, clear data */
			for (i = 0; i < iBlockSizeSam; i++)
				vecdAudioL[i] = vecdAudioR[i] = 0.0;
		}

		/* resample data for each channel separately */
		ResampleObjUpL.Resample(vecdAudioL, vecdAudioSndCrdL);
		ResampleObjUpR.Resample(vecdAudioR, vecdAudioSndCrdR);

		/* copy data from one stereo buffer in two separate buffers */
		iInCnt = 0;
		for (i = 0; i < iSndCrdBlockSizeSam; i++)
		{
			vecsAudioSndCrd[iInCnt++] = Double2Short(vecdAudioSndCrdL[i]);
			vecsAudioSndCrd[iInCnt++] = Double2Short(vecdAudioSndCrdR[i]);
		}

		/* play the new block */
		if (Sound.Write(vecsAudioSndCrd))
			PostWinMessage(MS_SOUND_OUT, MUL_COL_LED_RED);
		else
			PostWinMessage(MS_SOUND_OUT, MUL_COL_LED_GREEN);


		/* update response time measurement --------------------------------- */
		/* add time difference */
		const QTime CurTime = QTime::currentTime();

		/* we want to calculate the standard deviation (we assume that the mean
		   is correct at the block period time) */
		const double dCurAddVal =
			( (double) TimeLastBlock.msecsTo ( CurTime ) - MIN_BLOCK_DURATION_MS );

/*
// TEST
static FILE* pFileTest = fopen("sti.dat", "w");
fprintf(pFileTest, "%e\n", dCurAddVal);
fflush(pFileTest);
*/

		RespTimeMoAvBuf.Add ( dCurAddVal * dCurAddVal ); /* add squared value */

		/* store old time value */
		TimeLastBlock = CurTime;
	}

	/* reset current signal level and LEDs */
	SignalLevelMeterL.Reset();
	SignalLevelMeterR.Reset();
	PostWinMessage(MS_RESET_ALL, 0);
}

bool CClient::Stop()
{
	/* set flag so that thread can leave the main loop */
	bRun = false;

	/* give thread some time to terminate, return status */
	return wait(5000);
}