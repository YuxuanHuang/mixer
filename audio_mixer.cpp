	bool FOutputBuffer::MixNextBuffer()
 	{
		// If the circular queue is already full, exit.
		if (CircularBuffer.Remainder() < static_cast<uint32>(RenderBuffer.Num()))
		{
			return false;
		}

		CSV_SCOPED_TIMING_STAT(Audio, RenderAudio);

		// Zero the buffer
		FPlatformMemory::Memzero(RenderBuffer.GetData(), RenderBuffer.Num() * sizeof(float));
		if (AudioMixer != nullptr)
		{
			AudioMixer->OnProcessAudioStream(RenderBuffer);
		}

		switch (DataFormat)
		{
		case EAudioMixerStreamDataFormat::Float:
		{
			if (!FMath::IsNearlyEqual(LinearGainScalarForFinalOututCVar, 1.0f))
			{
				MultiplyBufferByConstantInPlace(RenderBuffer, LinearGainScalarForFinalOututCVar);
			}
			BufferRangeClampFast(RenderBuffer, -1.0f, 1.0f);

			// No conversion is needed, so we push the RenderBuffer directly to the circular queue.
			CircularBuffer.Push(reinterpret_cast<const uint8*>(RenderBuffer.GetData()), RenderBuffer.Num() * sizeof(float));
		}
		break;

		case EAudioMixerStreamDataFormat::Int16:
		{
			int16* BufferInt16 = (int16*)FormattedBuffer.GetData();
			const int32 NumSamples = RenderBuffer.Num();
			check(FormattedBuffer.Num() / GetSizeForDataFormat(DataFormat) == RenderBuffer.Num());			

			const float ConversionScalar = LinearGainScalarForFinalOututCVar * 32767.0f;
			MultiplyBufferByConstantInPlace(RenderBuffer, ConversionScalar);
			BufferRangeClampFast(RenderBuffer, -32767.0f, 32767.0f);

			for (int32 i = 0; i < NumSamples; ++i)
			{
				BufferInt16[i] = (int16)RenderBuffer[i];
			}

			CircularBuffer.Push(reinterpret_cast<const uint8*>(FormattedBuffer.GetData()), FormattedBuffer.Num());
		}
		break;

		default:
			// Not implemented/supported
			check(false);
			break;
		}

		static const int32 HeartBeatRate = 500;
		if ((ExtraAudioMixerDeviceLoggingCVar > 0) && (++CallCounterMixNextBuffer > HeartBeatRate))
		{
			UE_LOG(LogAudioMixer, Display, TEXT("FOutputBuffer::MixNextBuffer() called %i times"), HeartBeatRate);
			CallCounterMixNextBuffer = 0;
		}

		return true;
 	}
