#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <vector>

#include "Codec.h"
#include "Element.h"
#include "InPin.h"


class NullSink : public Element
{
	InPinSPTR pin;


public:
	virtual void Initialize() override
	{
		ClearOutputPins();
		ClearInputPins();

		// Create a pin
		PinInfoSPTR info = std::make_shared<PinInfo>(MediaCategory::Unknown);

		ElementWPTR weakPtr = shared_from_this();
		pin = std::make_shared<InPin>(weakPtr, info);
		AddInputPin(pin);
	}

	virtual void DoWork() override
	{
		BufferSPTR buffer;
		while (pin->TryGetFilledBuffer(&buffer))
		{
			pin->PushProcessedBuffer(buffer);
		}

		pin->ReturnProcessedBuffers();
	}

	virtual void ChangeState(MediaState oldState, MediaState newState) override
	{
		Element::ChangeState(oldState, newState);
	}
};



class AudioCodecElement : public Element
{
	const int alsa_channels = 2;

	InPinSPTR audioInPin;
	OutPinSPTR audioOutPin;
	AudioPinInfoSPTR outInfo;

	AVCodec* soundCodec = nullptr;
	AVCodecContext* soundCodecContext = nullptr;
	bool isFirstData = true;
	AudioStreamType audioFormat = AudioStreamType::Unknown;

	int streamChannels = 0;
	int outputChannels = 0;
	int sampleRate = 0;
	AVFrameBufferSPTR frame = std::make_shared<AVFrameBuffer>(0);;


	void SetupCodec()
	{
		switch (audioFormat)
		{				
		case AudioStreamType::Aac:
			soundCodec = avcodec_find_decoder(AV_CODEC_ID_AAC);
			break;

		case AudioStreamType::Ac3:
			soundCodec = avcodec_find_decoder(AV_CODEC_ID_AC3);
			break;

		case AudioStreamType::Dts:
			soundCodec = avcodec_find_decoder(AV_CODEC_ID_DTS);
			break;

		case AudioStreamType::Mpeg2Layer3:
			soundCodec = avcodec_find_decoder(AV_CODEC_ID_MP3);
			break;

		//case AudioStreamType::Pcm:

		default:
			printf("Audio format %d is not supported.\n", (int)audioFormat);
			throw NotSupportedException();
		}

		if (!soundCodec)
		{
			throw Exception("codec not found\n");
		}


		soundCodecContext = avcodec_alloc_context3(soundCodec);
		if (!soundCodecContext)
		{
			throw Exception("avcodec_alloc_context3 failed.\n");
		}


		soundCodecContext->channels = alsa_channels;
		soundCodecContext->sample_rate = sampleRate;
		//soundCodecContext->sample_fmt = AV_SAMPLE_FMT_S16P; //AV_SAMPLE_FMT_FLTP; //AV_SAMPLE_FMT_S16P
		soundCodecContext->request_sample_fmt = AV_SAMPLE_FMT_FLTP; // AV_SAMPLE_FMT_S16P; //AV_SAMPLE_FMT_FLTP;
		soundCodecContext->request_channel_layout = AV_CH_LAYOUT_STEREO;

		/* open it */
		if (avcodec_open2(soundCodecContext, soundCodec, NULL) < 0)
		{
			throw Exception("could not open codec\n");
		}
	}


	void ProcessBuffer(AVPacketBufferSPTR buffer)
	{
		AVPacket* pkt = buffer->GetAVPacket();
		AVFrame* decoded_frame = frame->GetAVFrame();


		// Decode audio
		//printf("Decoding frame (AVPacket=%p, size=%d).\n",
		//	buffer->GetAVPacket(), buffer->GetAVPacket()->size);

		int bytesDecoded = 0;
		while (bytesDecoded < pkt->size)
		{
			int got_frame = 0;
			int len = avcodec_decode_audio4(soundCodecContext,
				decoded_frame,
				&got_frame,
				pkt);

			//printf("avcodec_decode_audio4 len=%d\n", len);

			if (len < 0)
			{
				// Report the error, but otherwise ignore it.				
				char errmsg[1024] = { 0 };
				av_strerror(len, errmsg, 1024);

				Log("Error while decoding: %s\n", errmsg);

				break;
			}
			else
			{
				bytesDecoded += len;
			}

			Log("decoded audio frame OK (len=%x, pkt.size=%x)\n", len, buffer->GetAVPacket()->size);


			// Convert audio to ALSA format
			if (got_frame)
			{
				// Copy out the PCM data because libav fills the frame
				// with re-used data pointers.
				PcmFormat format;
				switch (decoded_frame->format)
				{
				case AV_SAMPLE_FMT_S16P:
					format = PcmFormat::Int16Planes;
					break;

				case AV_SAMPLE_FMT_FLTP:
					format = PcmFormat::Float32Planes;
					break;

				default:
					throw NotSupportedException();
				}

				PcmDataBufferSPTR pcmDataBuffer = std::make_shared<PcmDataBuffer>(
					(void*)this,
					format,
					decoded_frame->channels,
					decoded_frame->nb_samples);
				
				pcmDataBuffer->SetTimeStamp(
					av_frame_get_best_effort_timestamp(frame->GetAVFrame()) *
					av_q2d(buffer->TimeBase()));
				
				//printf("decodec audio frame pts=%f\n", pcmDataBuffer->TimeStamp());


				int leftChannelIndex = av_get_channel_layout_channel_index(
					decoded_frame->channel_layout,
					AV_CH_FRONT_LEFT);

				int rightChannelIndex = av_get_channel_layout_channel_index(
					decoded_frame->channel_layout,
					AV_CH_FRONT_RIGHT);

				int centerChannelIndex = av_get_channel_layout_channel_index(
					decoded_frame->channel_layout,
					AV_CH_FRONT_CENTER);

				void* channels[3];
				channels[0] = (void*)decoded_frame->data[leftChannelIndex];
				channels[1] = (void*)decoded_frame->data[rightChannelIndex];

				if (decoded_frame->channels > 2)
				{
					channels[2] = (void*)decoded_frame->data[centerChannelIndex];
				}
				else
				{
					channels[2] = nullptr;
				}

				for (int i = 0; i < decoded_frame->channels; ++i)
				{
					PcmData* pcmData = pcmDataBuffer->GetPcmData();
					memcpy(pcmData->Channel[i], channels[i], pcmData->ChannelSize);
				}

				audioOutPin->SendBuffer(pcmDataBuffer);
			}
		}
	}


public:

	virtual void Initialize() override
	{
		ClearOutputPins();
		ClearInputPins();

		// TODO: Pin format negotiation

		{
			// Create an audio in pin
			AudioPinInfoSPTR info = std::make_shared<AudioPinInfo>();
			info->StreamType = AudioStreamType::Unknown;
			info->Channels = 0;
			info->SampleRate = 0;

			ElementWPTR weakPtr = shared_from_this();
			audioInPin = std::make_shared<InPin>(weakPtr, info);
			AddInputPin(audioInPin);
		}

		{
			// Create an audio out pin
			outInfo = std::make_shared<AudioPinInfo>();
			outInfo->StreamType = AudioStreamType::Unknown;
			outInfo->Channels = 0;
			outInfo->SampleRate = 0;

			ElementWPTR weakPtr = shared_from_this();
			audioOutPin = std::make_shared<OutPin>(weakPtr, outInfo);
			AddOutputPin(audioOutPin);
		}
	}

	virtual void DoWork() override
	{
		BufferSPTR buffer;

		// Reap output buffers
		while (audioOutPin->TryGetAvailableBuffer(&buffer))
		{
			// New buffers are created as needed so just
			// drop this buffer.
			Wake();
		}


		// Process inputs buffers
		while (audioInPin->TryGetFilledBuffer(&buffer))
		{
			if (isFirstData)
			{
				OutPinSPTR otherPin = audioInPin->Source();
				if (otherPin)
				{
					if (otherPin->Info()->Category() != MediaCategory::Audio)
					{
						throw InvalidOperationException("AlsaAudioSinkElement: Not connected to an audio pin.");
					}

					AudioPinInfoSPTR info = std::static_pointer_cast<AudioPinInfo>(otherPin->Info());
					audioFormat = info->StreamType;
					sampleRate = info->SampleRate;
					streamChannels = info->Channels;

					outInfo->SampleRate = info->SampleRate;
					outInfo->Channels = info->Channels;

					printf("AudioCodecElement: outInfo->SampleRate=%d, outInfo->Channels=%d\n", outInfo->SampleRate, outInfo->Channels);

					SetupCodec();

					isFirstData = false;
				}
			}


			AVPacketBufferSPTR avPacketBuffer = std::static_pointer_cast<AVPacketBuffer>(buffer);

			ProcessBuffer(avPacketBuffer);

			audioInPin->PushProcessedBuffer(buffer);
			audioInPin->ReturnProcessedBuffers();
		}

		
	}

	virtual void ChangeState(MediaState oldState, MediaState newState) override
	{
		Element::ChangeState(oldState, newState);
	}

};
