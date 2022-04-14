	void FMixerSubmix::ProcessAudio(AlignedFloatBuffer& OutAudioBuffer)
	{
		AUDIO_MIXER_CHECK_AUDIO_PLAT_THREAD(MixerDevice);

		// If this is a Soundfield Submix, process our soundfield and decode it to a OutAudioBuffer.
		if (IsSoundfieldSubmix())
		{
			FScopeLock ScopeLock(&SoundfieldStreams.StreamsLock);

			// Initialize or clear the mixed down audio packet.
			if (!SoundfieldStreams.MixedDownAudio.IsValid())
			{
				SoundfieldStreams.MixedDownAudio = SoundfieldStreams.Factory->CreateEmptyPacket();
			}
			else
			{
				SoundfieldStreams.MixedDownAudio->Reset();
			}

			check(SoundfieldStreams.MixedDownAudio.IsValid());

			ProcessAudio(*SoundfieldStreams.MixedDownAudio);

			if (!SoundfieldStreams.ParentDecoder.IsValid())
			{
				return;
			}

			//Decode soundfield to interleaved float audio.
			FSoundfieldDecoderInputData DecoderInput =
			{
				*SoundfieldStreams.MixedDownAudio, /* SoundfieldBuffer */
				SoundfieldStreams.CachedPositionalData, /* PositionalData */
				MixerDevice ? MixerDevice->GetNumOutputFrames() : 0, /* NumFrames */
				MixerDevice ? MixerDevice->GetSampleRate() : 0.0f /* SampleRate */
			};

			FSoundfieldDecoderOutputData DecoderOutput = { OutAudioBuffer };

			SoundfieldStreams.ParentDecoder->DecodeAndMixIn(DecoderInput, DecoderOutput);
			return;
		}
		else
		{
			// Pump pending command queues. For Soundfield Submixes this occurs in ProcessAudio(ISoundfieldAudioPacket&).
			PumpCommandQueue();
		}

		// Device format may change channels if device is hot swapped
		NumChannels = MixerDevice->GetNumDeviceChannels();

		// If we hit this, it means that platform info gave us an invalid NumChannel count.
		if (!ensure(NumChannels != 0 && NumChannels <= AUDIO_MIXER_MAX_OUTPUT_CHANNELS))
		{
			return;
		}

		const int32 NumOutputFrames = OutAudioBuffer.Num() / NumChannels;
		NumSamples = NumChannels * NumOutputFrames;

 		InputBuffer.Reset(NumSamples);
 		InputBuffer.AddZeroed(NumSamples);

		float* BufferPtr = InputBuffer.GetData();

		// Mix all submix audio into this submix's input scratch buffer
		{
			CSV_SCOPED_TIMING_STAT(Audio, SubmixChildren);

			// First loop this submix's child submixes mixing in their output into this submix's dry/wet buffers.
			TArray<uint32> ToRemove;
			for (auto& ChildSubmixEntry : ChildSubmixes)
			{
				TSharedPtr<Audio::FMixerSubmix, ESPMode::ThreadSafe> ChildSubmix = ChildSubmixEntry.Value.SubmixPtr.Pin();
				if (ChildSubmix.IsValid())
				{
					ChildSubmix->ProcessAudio(InputBuffer);
				}
				else
				{
					ToRemove.Add(ChildSubmixEntry.Key);
				}
			}

			for (uint32 Key : ToRemove)
			{
				ChildSubmixes.Remove(Key);
			}
		}

		{
			CSV_SCOPED_TIMING_STAT(Audio, SubmixSource);

			// Loop through this submix's sound sources
			for (const auto& MixerSourceVoiceIter : MixerSourceVoices)
			{
				const FMixerSourceVoice* MixerSourceVoice = MixerSourceVoiceIter.Key;
				const float SendLevel = MixerSourceVoiceIter.Value.SendLevel;
				const EMixerSourceSubmixSendStage SubmixSendStage = MixerSourceVoiceIter.Value.SubmixSendStage;

				MixerSourceVoice->MixOutputBuffers(NumChannels, SendLevel, SubmixSendStage, InputBuffer);
			}
		}

		DryChannelBuffer.Reset();

		// Check if we need to allocate a dry buffer. This is stored here before effects processing. We mix in with wet buffer after effects processing.
		if (!FMath::IsNearlyEqual(CurrentDryLevel, TargetDryLevel) || !FMath::IsNearlyZero(CurrentDryLevel))
		{
			DryChannelBuffer.Append(InputBuffer);
		}

		{
			FScopeLock ScopeLock(&EffectChainMutationCriticalSection);

			if (!BypassAllSubmixEffectsCVar && EffectChains.Num() > 0)
			{		
				CSV_SCOPED_TIMING_STAT(Audio, SubmixEffectProcessing);

				float SampleRate = MixerDevice->GetSampleRate();
				check(SampleRate > 0.0f);
				float DeltaTimeSec = NumOutputFrames / SampleRate;

				// Setup the input data buffer
				FSoundEffectSubmixInputData InputData;
				InputData.AudioClock = MixerDevice->GetAudioTime();

				// Compute the number of frames of audio. This will be independent of if we downmix our wet buffer.
				InputData.NumFrames = NumSamples / NumChannels;
				InputData.NumChannels = NumChannels;
				InputData.NumDeviceChannels = MixerDevice->GetNumDeviceChannels();
				InputData.ListenerTransforms = MixerDevice->GetListenerTransforms();
				InputData.AudioClock = MixerDevice->GetAudioClock();

				SubmixChainMixBuffer.Reset(NumSamples);
				SubmixChainMixBuffer.AddZeroed(NumSamples);
				bool bProcessedAnEffect = false;

				for (int32 EffectChainIndex = EffectChains.Num() - 1; EffectChainIndex >= 0; --EffectChainIndex)
				{
					FSubmixEffectFadeInfo& FadeInfo = EffectChains[EffectChainIndex];

					if (!FadeInfo.EffectChain.Num())
					{
						continue;
					}

					// If we're not the current chain and we've finished fading out, lets remove it from the effect chains
					if (!FadeInfo.bIsCurrentChain && FadeInfo.FadeVolume.IsDone())
					{
						// only remove effect chain if it's not the base effect chain
						if (!FadeInfo.bIsBaseEffect)
						{
							EffectChains.RemoveAtSwap(EffectChainIndex, 1, true);
						}
						continue;
					}

					// Prepare the scratch buffer for effect chain processing
					EffectChainOutputBuffer.SetNumUninitialized(NumSamples);

					bProcessedAnEffect |= GenerateEffectChainAudio(InputData, InputBuffer, FadeInfo.EffectChain, EffectChainOutputBuffer);

					float StartFadeVolume = FadeInfo.FadeVolume.GetValue();
					FadeInfo.FadeVolume.Update(DeltaTimeSec);
					float EndFadeVolume = FadeInfo.FadeVolume.GetValue();

					MixInBufferFast(EffectChainOutputBuffer, SubmixChainMixBuffer, StartFadeVolume, EndFadeVolume);
				}

				// If we processed any effects, write over the old input buffer vs mixing into it. This is basically the "wet channel" audio in a submix.
				if (bProcessedAnEffect)
				{
					FMemory::Memcpy((void*)BufferPtr, (void*)SubmixChainMixBuffer.GetData(), sizeof(float)* NumSamples);
				}

				// Apply the wet level here after processing effects. 
				if (!FMath::IsNearlyEqual(TargetWetLevel, CurrentWetLevel) || !FMath::IsNearlyEqual(CurrentWetLevel, 1.0f))
				{
					if (FMath::IsNearlyEqual(TargetWetLevel, CurrentWetLevel))
					{
						MultiplyBufferByConstantInPlace(InputBuffer, TargetWetLevel);
					}
					else
					{
						FadeBufferFast(InputBuffer, CurrentWetLevel, TargetWetLevel);
						CurrentWetLevel = TargetWetLevel;
					}
				}
			}
		}

		// Mix in the dry channel buffer
		if (DryChannelBuffer.Num() > 0)
		{
			// If we've already set the volume, only need to multiply by constant
			if (FMath::IsNearlyEqual(TargetDryLevel, CurrentDryLevel))
			{
				MultiplyBufferByConstantInPlace(DryChannelBuffer, TargetDryLevel);
			}
			else
			{
				// To avoid popping, we do a fade on the buffer to the target volume
				FadeBufferFast(DryChannelBuffer, CurrentDryLevel, TargetDryLevel);
				CurrentDryLevel = TargetDryLevel;
			}
			MixInBufferFast(DryChannelBuffer, InputBuffer);
		}

		// If we're muted, memzero the buffer. Note we are still doing all the work to maintain buffer state between mutings.
		if (bIsBackgroundMuted)
		{
			FMemory::Memzero((void*)BufferPtr, sizeof(float) * NumSamples);
		}
	
		// If we are recording, Add out buffer to the RecordingData buffer:
		{
			FScopeLock ScopedLock(&RecordingCriticalSection);
			if (bIsRecording)
			{
				// TODO: Consider a scope lock between here and OnStopRecordingOutput.
				RecordingData.Append((float*)BufferPtr, NumSamples);
			}
		}

		// If spectrum analysis is enabled for this submix, downmix the resulting audio
		// and push it to the spectrum analyzer.
		{
			FScopeTryLock TryLock(&SpectrumAnalyzerCriticalSection);

			if (TryLock.IsLocked() && SpectrumAnalyzer.IsValid())
			{
				MixBufferDownToMono(InputBuffer, NumChannels, MonoMixBuffer);
				SpectrumAnalyzer->PushAudio(MonoMixBuffer.GetData(), MonoMixBuffer.Num());
				SpectrumAnalyzer->PerformAnalysisIfPossible(true, true);
			}
		}

		// Perform any envelope following if we're told to do so
		if (bIsEnvelopeFollowing)
		{
			const int32 BufferSamples = InputBuffer.Num();
			const float* AudioBufferPtr = InputBuffer.GetData();

			// Perform envelope following per channel
			FScopeLock EnvelopeScopeLock(&EnvelopeCriticalSection);
			FMemory::Memset(EnvelopeValues, sizeof(float) * AUDIO_MIXER_MAX_OUTPUT_CHANNELS);

			for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
			{
				// Get the envelope follower for the channel
				FEnvelopeFollower& EnvFollower = EnvelopeFollowers[ChannelIndex];

				// Track the last sample
				for (int32 SampleIndex = ChannelIndex; SampleIndex < BufferSamples; SampleIndex += NumChannels)
				{
					const float SampleValue = AudioBufferPtr[SampleIndex];
					EnvFollower.ProcessAudio(SampleValue);
				}

				EnvelopeValues[ChannelIndex] = EnvFollower.GetCurrentValue();
			}

			EnvelopeNumChannels = NumChannels;
		}

		// Now apply the output volume
		if (!FMath::IsNearlyEqual(TargetOutputVolume, CurrentOutputVolume) || !FMath::IsNearlyEqual(CurrentOutputVolume, 1.0f))
		{
			// If we've already set the output volume, only need to multiply by constant
			if (FMath::IsNearlyEqual(TargetOutputVolume, CurrentOutputVolume))
			{
				Audio::MultiplyBufferByConstantInPlace(InputBuffer, TargetOutputVolume);
			}
			else
			{
				// To avoid popping, we do a fade on the buffer to the target volume
				Audio::FadeBufferFast(InputBuffer, CurrentOutputVolume, TargetOutputVolume);
				CurrentOutputVolume = TargetOutputVolume;
			}
		}

		// Mix the audio buffer of this submix with the audio buffer of the output buffer (i.e. with other submixes)
		Audio::MixInBufferFast(InputBuffer, OutAudioBuffer);

		// Now loop through any buffer listeners and feed the listeners the result of this audio callback
		if(const USoundSubmix* SoundSubmix = Cast<const USoundSubmix>(OwningSubmixObject))
		{
			CSV_SCOPED_TIMING_STAT(Audio, SubmixBufferListeners);
			double AudioClock = MixerDevice->GetAudioTime();
			float SampleRate = MixerDevice->GetSampleRate();
			FScopeLock Lock(&BufferListenerCriticalSection);
			for (ISubmixBufferListener* BufferListener : BufferListeners)
			{
				check(BufferListener);
				BufferListener->OnNewSubmixBuffer(SoundSubmix, OutAudioBuffer.GetData(), OutAudioBuffer.Num(), NumChannels, SampleRate, AudioClock);
			}
		}
	}

	void FMixerSubmix::MixInChildSubmix(FChildSubmixInfo& Child, ISoundfieldAudioPacket& PacketToSumTo)
	{
		check(IsSoundfieldSubmix());

		// We only either encode, transcode input, and never both. If we have both for this child, something went wrong in initialization.
		check(!(Child.Encoder.IsValid() && Child.Transcoder.IsValid()));

		TSharedPtr<FMixerSubmix, ESPMode::ThreadSafe> ChildSubmixSharedPtr = Child.SubmixPtr.Pin();
		if (ChildSubmixSharedPtr.IsValid())
		{
			if (!ChildSubmixSharedPtr->IsSoundfieldSubmix())
			{
				// Reset the output scratch buffer so that we can call ProcessAudio on the ChildSubmix with it:
				ScratchBuffer.Reset(NumSamples);
				ScratchBuffer.AddZeroed(NumSamples);

				// If this is true, the Soundfield Factory explicitly requested that a seperate encoder stream was set up for every
				// non-soundfield child submix.
				if (Child.Encoder.IsValid())
				{					
					ChildSubmixSharedPtr->ProcessAudio(ScratchBuffer);

					// Encode the resulting audio and mix it in.
					FSoundfieldEncoderInputData InputData = {
						ScratchBuffer, /* AudioBuffer */
						ChildSubmixSharedPtr->NumChannels, /* NumChannels */
						*SoundfieldStreams.Settings, /** InputSettings */
						SoundfieldStreams.CachedPositionalData /** PosititonalData */
					};

					Child.Encoder->EncodeAndMixIn(InputData, PacketToSumTo);
				}
				else
				{
					// Otherwise, process and mix in the submix's audio to the scratch buffer, and we will encode ScratchBuffer later.
					ChildSubmixSharedPtr->ProcessAudio(ScratchBuffer);
				}
			}
			else if (Child.Transcoder.IsValid())
			{
				// Make sure our packet that we call process on is zeroed out:
				if (!Child.IncomingPacketToTranscode.IsValid())
				{
					Child.IncomingPacketToTranscode = ChildSubmixSharedPtr->SoundfieldStreams.Factory->CreateEmptyPacket();
				}
				else
				{
					Child.IncomingPacketToTranscode->Reset();
				}

				check(Child.IncomingPacketToTranscode.IsValid());

				ChildSubmixSharedPtr->ProcessAudio(*Child.IncomingPacketToTranscode);

				Child.Transcoder->TranscodeAndMixIn(*Child.IncomingPacketToTranscode, ChildSubmixSharedPtr->GetSoundfieldSettings(), PacketToSumTo, *SoundfieldStreams.Settings);
			}
			else
			{
				// No conversion necessary.
				ChildSubmixSharedPtr->ProcessAudio(PacketToSumTo);
			}

			//Propogate listener rotation down to this submix.
			// This is required if this submix doesn't have any sources sending to it, but does have at least one child submix.
			UpdateListenerRotation(ChildSubmixSharedPtr->SoundfieldStreams.CachedPositionalData.Rotation);
		}
	}
