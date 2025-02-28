#include "executionhandler.h"

#include "runtime/emulation/cpuframecapture.h"
#include "runtime/emulation/cpumos6510.h"
#include "runtime/emulation/sid/sidproxy.h"

#include "runtime/environmentdefines.h"
#include "runtime/execution/flightrecorder.h"

#include "foundation/base/assert.h"
#include "foundation/platform/imutex.h"
#include "foundation/platform/iplatform.h"

#include "utils/configfile.h"
#include "utils/logging.h"
#include "utils/global.h"

#include <algorithm>
#include <iostream>


using namespace Foundation;
using namespace Utility;

namespace Emulation
{

	// clamp multiplied samples to these limits
	const float sampleCeiling = 32767.0f;
	const float sampleFloor = -32767.0f;

	ExecutionHandler::ExecutionHandler(
		CPUmos6510* inCPU,
		CPUMemory* pMemory,
		SIDProxy* pSIDProxy,
		FlightRecorder* inFlightRecorder)
		: m_CPU(inCPU)
		, m_Memory(pMemory)
		, m_SIDProxy(pSIDProxy)
		, m_SIDRegisterFlightRecorder(inFlightRecorder)
		, m_IsStarted(false)
		, m_FeedCount(0)
		, m_BytesFedCount(0)
		, m_SampleBufferReadCursor(0)
		, m_SampleBufferWriteCursor(0)
		, m_CPUFrameCounter(0)
		, m_UpdateEnabled(false)
    , m_ErrorState(false)
	{
		m_CyclesPerFrame = EMULATION_CYCLES_PER_FRAME_PAL;

		// Create a sample buffer. The sample frequency is used for determining the size, which is probably 50 times the size required.
		m_SampleBufferSize = (static_cast<unsigned int>(pSIDProxy->GetSampleFrequency()) << 8);
		m_SampleBuffer = new short[m_SampleBufferSize];
		m_Mutex = Global::instance().GetPlatform().CreateMutex();
		m_OutputGain = GetSingleConfigurationValue<Utility::Config::ConfigValueFloat>(Global::instance().GetConfig(), "Sound.Output.Gain", -1.0f);

		Logging::instance().Info("Sound.Output.Gain = %f", m_OutputGain);
		// Set default action vector
		m_InitVector = 0x1000;
		m_StopVector = 0x1003;
		m_UpdateVector = 0x1006;
	}

	ExecutionHandler::~ExecutionHandler()
	{
		m_Mutex = nullptr;

		if (m_SampleBuffer != nullptr)
			delete[] m_SampleBuffer;
	}

	//----------------------------------------------------------------------------------------------------------------
	// IAudioStreamFeeder
	//----------------------------------------------------------------------------------------------------------------

	void ExecutionHandler::Start()
	{
		if (!m_IsStarted)
		{
			m_FeedCount = 0;
			m_BytesFedCount = 0;
			m_CPUCyclesSpend = 0;
			m_CPUFrameCounter = 0;

			if (m_SIDProxy != nullptr)
				m_SIDProxy->Reset();

			if (m_SIDRegisterFlightRecorder != nullptr)
			{
				m_SIDRegisterFlightRecorder->Lock();
				m_SIDRegisterFlightRecorder->Reset();
				m_SIDRegisterFlightRecorder->Unlock();
			}

			m_IsStarted = true;
		}
	}

	void ExecutionHandler::Stop()
	{
		if (m_IsStarted)
		{
			m_SampleBufferReadCursor = 0;
			m_SampleBufferWriteCursor = 0;

			m_IsStarted = false;
		}
	}

	//----------------------------------------------------------------------------------------------------------------

	bool ExecutionHandler::IsStarted() const
	{
		return m_IsStarted;
	}

	//----------------------------------------------------------------------------------------------------------------

	unsigned int ExecutionHandler::GetBytesFed() const
	{
		return m_BytesFedCount;
	}

	unsigned int ExecutionHandler::GetFeedCount() const
	{
		return m_FeedCount;
	}

	//----------------------------------------------------------------------------------------------------------------

	void ExecutionHandler::PreFeedPCM(void* inBuffer, unsigned int inByteCount)
	{
		FeedPCM(inBuffer, inByteCount);
	}

	void ExecutionHandler::FeedPCM(void* inBuffer, unsigned int inByteCount)
	{
		m_FeedCount++;
		m_BytesFedCount += inByteCount;

		if (!m_IsStarted)
		{
			memset(inBuffer, 0, inByteCount);
		}
		else
		{
			unsigned int uiRemainingSamples = (inByteCount >> 1);

			short* pSource = static_cast<short*>(m_SampleBuffer);
			short* pTarget = static_cast<short*>(inBuffer);

			while (uiRemainingSamples > 0)
			{
				FOUNDATION_ASSERT(m_SampleBufferReadCursor <= m_SampleBufferWriteCursor);

				if (m_SampleBufferReadCursor >= m_SampleBufferWriteCursor)
				{
					// Capture a single frame of audio
					CaptureNewFrame();
				}

				const unsigned int uiRemainingSourceSamples = m_SampleBufferWriteCursor - m_SampleBufferReadCursor;
				const unsigned int uiSamplesToCopy = uiRemainingSamples > uiRemainingSourceSamples ? uiRemainingSourceSamples : uiRemainingSamples;

				for (unsigned int i = 0; i < uiSamplesToCopy; ++i)
				{
					const float fSample = static_cast<float>(pSource[i + m_SampleBufferReadCursor]) * m_OutputGain;
					const float fClampedSample = fmin(sampleCeiling, fmax(fSample, sampleFloor));
					pTarget[i] = static_cast<short>(fClampedSample);
				}

				// Forward the read cursor
				m_SampleBufferReadCursor += uiSamplesToCopy;

				// Forward the target pointer
				pTarget += uiSamplesToCopy;

				// Decrement the remaining number of samples
				uiRemainingSamples -= uiSamplesToCopy;
			}
		}
	}

	//----------------------------------------------------------------------------------------------------------------
	// Lock and unlock
	//----------------------------------------------------------------------------------------------------------------

	void ExecutionHandler::Lock()
	{
		if (m_Mutex != nullptr)
			m_Mutex->Lock();
	}

	void ExecutionHandler::Unlock()
	{
		if (m_Mutex != nullptr)
			m_Mutex->Unlock();
	}

	//----------------------------------------------------------------------------------------------------------------

	void ExecutionHandler::SetPAL(const bool inPALMode)
	{
		m_CyclesPerFrame = inPALMode ? EMULATION_CYCLES_PER_FRAME_PAL : EMULATION_CYCLES_PER_FRAME_NTSC;
	}


	//----------------------------------------------------------------------------------------------------------------
	// Error
	//----------------------------------------------------------------------------------------------------------------

	bool ExecutionHandler::IsInErrorState() const
	{
		return m_ErrorState;
	}

	std::string ExecutionHandler::GetErrorMessage() const
	{
		return m_ErrorMessage;
	}

	void ExecutionHandler::ClearErrorState()
	{
		m_ErrorState = false;
	}


	//----------------------------------------------------------------------------------------------------------------
	// Emulation update
	//----------------------------------------------------------------------------------------------------------------

	void ExecutionHandler::SetEnableUpdate(bool inEnableUpdate)
	{
		Lock();
		m_UpdateEnabled = inEnableUpdate;
		Unlock();
	}

	void ExecutionHandler::SetFastForward(unsigned int inFastForwardUpdateCount)
	{
		Lock();
		m_FastForwardUpdateCount = inFastForwardUpdateCount;
		Unlock();
	}

	void ExecutionHandler::QueueInit(unsigned char inInitArgument)
	{
		Lock();
		m_ActionQueue.push_back({ ActionType::Init, inInitArgument });
		Unlock();
	}

	void ExecutionHandler::QueueInit(unsigned char inInitArgument, const std::function<void(CPUMemory*)>& inPostInitCallback)
	{
		Lock();
		m_ActionQueue.push_back({ ActionType::Init, inInitArgument });
		m_ActionQueue.push_back({ ActionType::Update, 0, inPostInitCallback });
		Unlock();
	}


	void ExecutionHandler::QueueStop()
	{
		Lock();
		m_ActionQueue.push_back({ ActionType::Stop, 0 });
		Unlock();
	}


	void ExecutionHandler::QueueMuteChannel(unsigned char inChannel, const std::function<void(CPUMemory*)>& inMuteCallback)
	{
		Lock();
		m_ActionQueue.push_back({ ActionType::ApplyMuteState, inChannel, inMuteCallback });
		Unlock();
	}


	void ExecutionHandler::QueueClearAllMuteState(const std::function<void(CPUMemory*)>& inClearMuteStateCallback)
	{
		Lock();
		m_ActionQueue.push_back({ ActionType::ClearMuteAllState, 0, inClearMuteStateCallback });
		Unlock();
	}


	void ExecutionHandler::SetInitVector(unsigned short inVector)
	{
		Lock();
		m_InitVector = inVector;
		Unlock();
	}

	void ExecutionHandler::SetStopVector(unsigned short inVector)
	{
		Lock();
		m_StopVector = inVector;
		Unlock();
	}

	void ExecutionHandler::SetUpdateVector(unsigned short inVector)
	{
		Lock();
		m_UpdateVector = inVector;
		Unlock();
	}

	void ExecutionHandler::SetPostUpdateCallback(const std::function<void(CPUMemory*)>& inPostUpdateCallback)
	{
		Lock();
		m_PostUpdateCallback = inPostUpdateCallback;
		Unlock();
	}

	void ExecutionHandler::StartWriteOutputToFile(const std::string& inFilename)
	{
		FOUNDATION_ASSERT(m_SIDProxy != nullptr);
		Lock();
		m_SIDProxy->StartRecordToFile(inFilename);
		Unlock();
	}

	void ExecutionHandler::StopWriteOutputToFile()
	{
		FOUNDATION_ASSERT(m_SIDProxy != nullptr);
		Lock();
		m_SIDProxy->StopRecordToFile();
		Unlock();
	}

	bool ExecutionHandler::IsWritingOutputToFile() const
	{
		FOUNDATION_ASSERT(m_SIDProxy != nullptr);
		return m_SIDProxy->IsRecordingToFile();
	}

	//----------------------------------------------------------------------------------------------------------------

	const unsigned short ExecutionHandler::GetAddressFromActionType(ActionType inActionType) const
	{
		switch (inActionType)
		{
		case ActionType::Init:
			return m_InitVector;
		case ActionType::Stop:
			return m_StopVector;
		case ActionType::Update:
			return m_UpdateVector;
		default:
			break;
		}

		FOUNDATION_ASSERT(false);

		return 0x0000;
	}

	//----------------------------------------------------------------------------------------------------------------

	void ExecutionHandler::SimulateSID(int inDeltaCycles)
	{
		short* pSampleBuffer = static_cast<short*>(m_SampleBuffer);

		//while (inDeltaCycles > 0)
		{
			// Get offset pointer inside the sample buffer
			short* sample_buffer_write_location = pSampleBuffer + m_SampleBufferWriteCursor;

			// Calculate remaining samples in the buffer, so that we do not overflow it!
			const int uiRemainingSamplesInBuffer = m_SampleBufferSize - m_SampleBufferWriteCursor;

			// Clock the sid
			const int nSamplesWritten = m_SIDProxy->Clock(inDeltaCycles, sample_buffer_write_location, uiRemainingSamplesInBuffer);

			// Negative sample count written is invalid!
			FOUNDATION_ASSERT(nSamplesWritten >= 0);

			// Move the write cursor
			m_SampleBufferWriteCursor += static_cast<unsigned int>(nSamplesWritten);

			// Make sure there's no overflow
			FOUNDATION_ASSERT(m_SampleBufferWriteCursor <= m_SampleBufferSize);
		}
	}

	void ExecutionHandler::CaptureNewFrame()
	{
		FOUNDATION_ASSERT(m_CPU != nullptr);

		// Lock execution handler
		Lock();

		// Lock memory access
		m_Memory->Lock();

		// Reset read/write cursor, and run simulation of the SID
		m_SampleBufferReadCursor = 0;
		m_SampleBufferWriteCursor = 0;

		// Attach memory to cpu
		m_CPU->SetMemory(m_Memory);

		// Capture the frame (this will run the CPU )
		CPUFrameCapture frameCapture(m_CPU, 0xd400, 0xd418, m_CyclesPerFrame);

		// Execute queued actions
		for (const Action& action : m_ActionQueue)
		{
			switch (action.m_ActionType)
			{
			case ActionType::ApplyMuteState:
			{
				const unsigned short offset = action.m_ActionArgument * 7;
				const unsigned short address = 0xd400 + offset;

				for (int i = 0; i < 7; ++i)
					frameCapture.Write(address + i, 0, 0);
			}
			break;
			case ActionType::ClearMuteAllState:
				break;
			case ActionType::Init:
			case ActionType::Stop:
				frameCapture.Capture(GetAddressFromActionType(action.m_ActionType), action.m_ActionArgument);
				break;
			case ActionType::Update:
				if (!m_ErrorState)
					frameCapture.Capture(GetAddressFromActionType(action.m_ActionType), action.m_ActionArgument);
			default:
				break;
			}

			if (action.m_PostActionCallback)
				action.m_PostActionCallback(m_Memory);
		}

		m_ActionQueue.clear();

		// Execute driver update, if enabled
		if (m_UpdateEnabled && !m_ErrorState)
		{
			bool error = frameCapture.IsMaxCycleCountReached();

			if (!error)
			{
				frameCapture.Capture(GetAddressFromActionType(ActionType::Update), 0);
				error = frameCapture.IsMaxCycleCountReached();

				if (m_PostUpdateCallback)
					m_PostUpdateCallback(m_Memory);
			}

			if (!error)
			{
				for (unsigned int i = 0; i < m_FastForwardUpdateCount; ++i)
				{
					// Break out if less than a quarter of the cycles of a frame remains
					if (m_CyclesPerFrame - frameCapture.GetCyclesSpend() < m_CyclesPerFrame >> 2)
						break;

					frameCapture.Capture(GetAddressFromActionType(ActionType::Update), 0);
					if (m_PostUpdateCallback)
						m_PostUpdateCallback(m_Memory);

					error = frameCapture.IsMaxCycleCountReached();

					if (error)
						break;
				}
			}

			if (error)
			{
				m_ErrorState = true;
				m_ErrorMessage = "Emulation of 6510 code exceeded cycle window!";
			}
		}

		// Increment frame counter
		m_CPUFrameCounter++;

		// Run the flight recorder
		if (m_SIDRegisterFlightRecorder != nullptr && m_SIDRegisterFlightRecorder->IsRecording())
		{
			m_SIDRegisterFlightRecorder->Lock();
			m_SIDRegisterFlightRecorder->Record(m_CPUFrameCounter, m_Memory, frameCapture.GetCyclesSpend());
			m_SIDRegisterFlightRecorder->Unlock();
		}

		// Unlock memory access
		m_Memory->Unlock();

		// Grab the cycle count of the CPU here, as this will be the number of cycles spend on the driver update
		m_CPUCyclesSpend = frameCapture.GetCyclesSpend();

		// Do all writes to the SID and emulate cycles spend
		int nCycle = 0;

		while (frameCapture.HasNext())
		{
			const CPUFrameCapture::WriteCapture& capture = frameCapture.GetNext();

			FOUNDATION_ASSERT(nCycle <= capture.m_iCycle);

			const int deltaCycles = capture.m_iCycle - nCycle;
			SimulateSID(deltaCycles);
			m_SIDProxy->Write((unsigned char)(capture.m_usReg & 0xff), capture.m_ucVal);
			nCycle += deltaCycles;
		}

		// Do the rest of the frame
		while (nCycle < (int)m_CyclesPerFrame)
		{
			const int deltaCycles = m_CyclesPerFrame - nCycle;
			SimulateSID(deltaCycles);
			nCycle += deltaCycles;
		}

		// Reset cycle counter
		m_CurrentCycle = 0;

		// Unlock execution handler
		Unlock();
	}
}
