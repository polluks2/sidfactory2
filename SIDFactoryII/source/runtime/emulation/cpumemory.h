#if !defined(__MEMORY_H__)
#define __MEMORY_H__

#include "foundation/base/assert.h"
#include "foundation/platform/iplatform.h"
#include "foundation/platform/imutex.h"
#include "runtime/emulation/imemoryrandomreadaccess.h"

namespace Emulation
{
	class IPlatformFactory;

	class CPUMemory : public IMemoryRandomReadAccess
	{
	public:
		CPUMemory(unsigned int inSize, Foundation::IPlatform* inPlatform);
		~CPUMemory();

		const unsigned char& operator[](int inAddress) const override
		{
			FOUNDATION_ASSERT(inAddress >= 0);
			FOUNDATION_ASSERT(inAddress < (int)m_nSize);
			FOUNDATION_ASSERT(m_IsLocked);

			return m_Memory[inAddress];
		}

		unsigned char& operator[](int inAddress)
		{
			FOUNDATION_ASSERT(inAddress >= 0);
			FOUNDATION_ASSERT(inAddress < (int)m_nSize);
			FOUNDATION_ASSERT(m_IsLocked);

			return m_Memory[inAddress];
		}

		void Lock();
		void Unlock();
		bool IsLocked() const;

		unsigned int GetSize() const { return m_nSize; }

		void Clear();

		void TakeSnapshot();
		void RestoreFromSnapshot();
		void FlushSnapshot();

		unsigned char GetByte(unsigned int inAddress) const override;
		unsigned short GetWord(unsigned int inAddress) const override;
		void GetData(unsigned int inAddress, void* inDestinationBuffer, unsigned int inDestinationBufferByteCount) const override;

		void SetByte(unsigned int inAddress, unsigned char inByteValue);
		void SetWord(unsigned int inAddress, unsigned short inWordValue);
		void SetData(unsigned int inAddress, const void* inSourceBuffer, unsigned int inSourceBufferByteCount);

		void Copy(unsigned int inSourceAddress, unsigned int inLength, unsigned int inDestinationAddress);
		void Set(unsigned char inValue, unsigned int inAddress, unsigned int inLength);

		unsigned int GetAddress(const void* inMemoryOffsetPointer) const 
		{
			unsigned int iAddress = static_cast<unsigned int>(static_cast<const unsigned char*>(inMemoryOffsetPointer) - m_Memory);

			FOUNDATION_ASSERT(iAddress < m_nSize);
			FOUNDATION_ASSERT(m_IsLocked);

			return iAddress;
		};

	private:
		std::shared_ptr<Foundation::IMutex> m_Mutex;

		bool m_IsLocked;

		unsigned int m_nSize;
		unsigned char* m_Memory;
		unsigned char* m_MemorySnapshot;
	};
}

#endif //__MEMORY_H__