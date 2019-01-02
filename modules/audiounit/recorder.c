/**
 * @file audiounit/recorder.c  AudioUnit input recorder
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "audiounit.h"


struct ausrc_st {
	const struct ausrc *as;      /* inheritance */
	struct audiosess_st *sess;
	AudioUnit au_in;
	AudioUnit au_conv;
	pthread_mutex_t mutex;
	int ch;
	uint32_t sampsz;
	double sampc_ratio;
	AudioBufferList *abl;
	ausrc_read_h *rh;
	void *arg;
};


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	pthread_mutex_lock(&st->mutex);
	st->rh = NULL;
	pthread_mutex_unlock(&st->mutex);

	AudioOutputUnitStop(st->au_in);
	AudioUnitUninitialize(st->au_in);
	AudioComponentInstanceDispose(st->au_in);

	AudioOutputUnitStop(st->au_conv);
	AudioUnitUninitialize(st->au_conv);
	AudioComponentInstanceDispose(st->au_conv);

	mem_deref(st->sess);

	pthread_mutex_destroy(&st->mutex);
}


static OSStatus input_callback(void *inRefCon,
			       AudioUnitRenderActionFlags *ioActionFlags,
			       const AudioTimeStamp *inTimeStamp,
			       UInt32 inBusNumber,
			       UInt32 inNumberFrames,
			       AudioBufferList *ioData)
{
	struct ausrc_st *st = inRefCon;
	AudioBufferList abl_in, abl_conv;
	UInt32 outNumberFrames;
	OSStatus ret;
	ausrc_read_h *rh;
	void *arg;

	(void)ioData;

	pthread_mutex_lock(&st->mutex);
	rh  = st->rh;
	arg = st->arg;
	pthread_mutex_unlock(&st->mutex);

	if (!rh)
		return 0;

	st->abl = &abl_in;

	abl_in.mNumberBuffers = 1;
	abl_in.mBuffers[0].mNumberChannels = st->ch;
	abl_in.mBuffers[0].mData = NULL;
	abl_in.mBuffers[0].mDataByteSize = inNumberFrames * st->sampsz;

	ret = AudioUnitRender(st->au_in,
			      ioActionFlags,
			      inTimeStamp,
			      inBusNumber,
			      inNumberFrames,
			      &abl_in);
	if (ret) {
		debug("audiounit: record: AudioUnitRender input error (%d)\n",
		      ret);
		return ret;
	}

	outNumberFrames = inNumberFrames * st->sampc_ratio;
	abl_conv.mNumberBuffers = 1;
	abl_conv.mBuffers[0].mNumberChannels = st->ch;
	abl_conv.mBuffers[0].mData = NULL;
	abl_conv.mBuffers[0].mDataByteSize = outNumberFrames * st->sampsz;

	ret = AudioUnitRender(st->au_conv,
			      ioActionFlags,
			      inTimeStamp,
			      0,
			      outNumberFrames,
			      &abl_conv);
	if (ret) {
		debug("audiounit: record: AudioUnitRender convert error (%d)\n",
		      ret);
		return ret;
	}

	rh(abl_conv.mBuffers[0].mData,
	   abl_conv.mBuffers[0].mDataByteSize/st->sampsz, arg);

	return 0;
}


static OSStatus convert_callback(void *inRefCon,
			       AudioUnitRenderActionFlags *ioActionFlags,
			       const AudioTimeStamp *inTimeStamp,
			       UInt32 inBusNumber,
			       UInt32 inNumberFrames,
			       AudioBufferList *ioData)
{
	struct ausrc_st *st = inRefCon;
	AudioBufferList *abl;

	abl  = st->abl;

	if (!abl)
		return EINVAL;

	ioData->mBuffers[0].mData = abl->mBuffers[0].mData;

	return noErr;
}


static void interrupt_handler(bool interrupted, void *arg)
{
	struct ausrc_st *st = arg;

	if (interrupted)
		AudioOutputUnitStop(st->au_in);
	else
		AudioOutputUnitStart(st->au_in);
}


int audiounit_recorder_alloc(struct ausrc_st **stp, const struct ausrc *as,
			     struct media_ctx **ctx,
			     struct ausrc_prm *prm, const char *device,
			     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	AudioStreamBasicDescription fmt, fmt_app;
	const AudioUnitElement inputBus = 1;
	const AudioUnitElement outputBus = 0;
	AURenderCallbackStruct cb_in, cb_conv;
	struct ausrc_st *st;
	const UInt32 enable = 1;
	const UInt32 disable = 0;
#if ! TARGET_OS_IPHONE
	UInt32 ausize = sizeof(AudioDeviceID);
	AudioDeviceID inputDevice;
	AudioObjectPropertyAddress auAddress = {
		kAudioHardwarePropertyDefaultInputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster };
#endif
	Float64 hw_srate = 0.0;
	UInt32 hw_size = sizeof(hw_srate);
	OSStatus ret = 0;
	int err;

	(void)ctx;
	(void)device;
	(void)errh;

	if (!stp || !as || !prm)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->as  = as;
	st->rh  = rh;
	st->arg = arg;
	st->ch  = prm->ch;

	st->sampsz = (uint32_t)aufmt_sample_size(prm->fmt);
	if (!st->sampsz) {
		err = ENOTSUP;
		goto out;
	}

	err = pthread_mutex_init(&st->mutex, NULL);
	if (err)
		goto out;

	err = audiosess_alloc(&st->sess, interrupt_handler, st);
	if (err)
		goto out;

	ret = AudioComponentInstanceNew(audiounit_comp_io, &st->au_in);
	if (ret)
		goto out;

	ret = AudioUnitSetProperty(st->au_in, kAudioOutputUnitProperty_EnableIO,
				   kAudioUnitScope_Input, inputBus,
				   &enable, sizeof(enable));

#if ! TARGET_OS_IPHONE
	ret = AudioUnitSetProperty(st->au_in, kAudioOutputUnitProperty_EnableIO,
				   kAudioUnitScope_Output, outputBus,
				   &disable, sizeof(disable));
	if (ret)
		goto out;

	ret = AudioObjectGetPropertyData(kAudioObjectSystemObject,
			&auAddress,
			0,
			NULL,
			&ausize,
			&inputDevice);
	if (ret)
		goto out;

	ret = AudioUnitSetProperty(st->au_in,
			kAudioOutputUnitProperty_CurrentDevice,
			kAudioUnitScope_Global,
			0,
			&inputDevice,
			sizeof(inputDevice));
	if (ret)
		goto out;
#endif

	ret = AudioUnitGetProperty(st->au_in,
				   kAudioUnitProperty_SampleRate,
				   kAudioUnitScope_Input,
				   inputBus,
				   &hw_srate,
				   &hw_size);
	if (ret)
		goto out;

	debug("audiounit: record hardware sample rate is now at %f Hz\n",
	      hw_srate);

	st->sampc_ratio = prm->srate / hw_srate;

	fmt.mSampleRate       = hw_srate;
	fmt.mFormatID         = kAudioFormatLinearPCM;
#if TARGET_OS_IPHONE
	fmt.mFormatFlags      = audiounit_aufmt_to_formatflags(prm->fmt)
		| kAudioFormatFlagsNativeEndian
		| kAudioFormatFlagIsPacked;
#else
	fmt.mFormatFlags      = audiounit_aufmt_to_formatflags(prm->fmt)
		| kLinearPCMFormatFlagIsPacked;
#endif
	fmt.mBitsPerChannel   = 8 * st->sampsz;
	fmt.mChannelsPerFrame = prm->ch;
	fmt.mBytesPerFrame    = st->sampsz * prm->ch;
	fmt.mFramesPerPacket  = 1;
	fmt.mBytesPerPacket   = st->sampsz * prm->ch;
	fmt.mReserved         = 0;

	ret = AudioUnitSetProperty(st->au_in, kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Output, inputBus,
				   &fmt, sizeof(fmt));
	if (ret)
		goto out;

	/* NOTE: done after desc */
	ret = AudioUnitInitialize(st->au_in);
	if (ret)
		goto out;

	cb_in.inputProc = input_callback;
	cb_in.inputProcRefCon = st;
	ret = AudioUnitSetProperty(st->au_in,
				   kAudioOutputUnitProperty_SetInputCallback,
				   kAudioUnitScope_Global, inputBus,
				   &cb_in, sizeof(cb_in));
	if (ret)
		goto out;

	fmt_app = fmt;
	fmt_app.mSampleRate = prm->srate;

	ret = AudioComponentInstanceNew(audiounit_comp_conv, &st->au_conv);
	if (ret) {
		warning("audiounit: record: AudioConverter failed (%d)\n",
			ret);
		goto out;
	}

	info("audiounit: record: enable resampler %.1f -> %u Hz\n",
	     hw_srate, prm->srate);

	ret = AudioUnitSetProperty(st->au_conv,
				   kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Input,
				   0,
				   &fmt,
				   sizeof(fmt));
	if (ret)
		goto out;

	ret = AudioUnitSetProperty(st->au_conv,
				   kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Output,
				   0,
				   &fmt_app,
				   sizeof(fmt_app));
	if (ret)
		goto out;

	cb_conv.inputProc = convert_callback;
	cb_conv.inputProcRefCon = st;

	ret = AudioUnitSetProperty(st->au_conv,
				   kAudioUnitProperty_SetRenderCallback,
				   kAudioUnitScope_Input,
				   0,
				   &cb_conv,
				   sizeof(cb_conv));
	if (ret)
		goto out;

	ret = AudioUnitInitialize(st->au_conv);
	if (ret)
		goto out;

	ret = AudioOutputUnitStart(st->au_in);
	if (ret)
		goto out;

 out:
	if (ret) {
		warning("audiounit: record failed: %d (%c%c%c%c)\n", ret,
			ret>>24, ret>>16, ret>>8, ret);
		err = ENODEV;
	}

	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
