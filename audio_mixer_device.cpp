bool FMixerDevice::OnProcessAudioStream(AlignedFloatBuffer& Output)
	{
		LLM_SCOPE(ELLMTag::AudioMixer);

		// This function could be called in a task manager, which means the thread ID may change between calls.
		ResetAudioRenderingThreadId();

		// Update the audio render thread time at the head of the render
		AudioThreadTimingData.AudioRenderThreadTime = FPlatformTime::Seconds() - AudioThreadTimingData.StartTime;

		// Pump the command queue to the audio render thread
		PumpCommandQueue();

		// update the clock manager
		QuantizedEventClockManager.Update(SourceManager->GetNumOutputFrames());

		// Compute the next block of audio in the source manager
		SourceManager->ComputeNextBlockOfSamples();

		FMixerSubmixWeakPtr MasterSubmix = GetMasterSubmix();
		{
			CSV_SCOPED_TIMING_STAT(Audio, Submixes);

			FMixerSubmixPtr MasterSubmixPtr = MasterSubmix.Pin();
			if (MasterSubmixPtr.IsValid())
			{
				// Process the audio output from the master submix
				MasterSubmixPtr->ProcessAudio(Output);
			}
		}

		{
			CSV_SCOPED_TIMING_STAT(Audio, EndpointSubmixes);
			FScopeLock ScopeLock(&EndpointSubmixesMutationLock);
			for (FMixerSubmixPtr& Submix : DefaultEndpointSubmixes)
			{
				// If this hit, a submix was added to the default submix endpoint array
				// even though it's not an endpoint, or a parent was set on an endpoint submix
				// and it wasn't removed from DefaultEndpointSubmixes.
				ensure(Submix->IsDefaultEndpointSubmix());

				// Any endpoint submixes that don't specify an endpoint
				// are summed into our master output.
				Submix->ProcessAudio(Output);
			}
			
			for (FMixerSubmixPtr& Submix : ExternalEndpointSubmixes)
			{
				// If this hit, a submix was added to the external submix endpoint array
				// even though it's not an endpoint, or a parent was set on an endpoint submix
				// and it wasn't removed from ExternalEndpointSubmixes.
				ensure(Submix->IsExternalEndpointSubmix());

				Submix->ProcessAudioAndSendToEndpoint();
			}
		}

		// Reset stopping sounds and clear their state after submixes have been mixed
		SourceManager->ClearStoppingSounds();

		// Do any debug output performing
		if (bDebugOutputEnabled)
		{
			SineOscTest(Output);
		}

		// Update the audio clock
		AudioClock += AudioClockDelta;

		return true;
	}
