#include "fft_mirror_common.hlsl"

[[vk::binding(2, 0)]] RWTexture2D<float32_t4> convolvedImage;

// ---------------------------------------------------- Utils ---------------------------------------------------------
uint64_t rowMajorOffset(uint32_t x, uint32_t y)
{
	return y * pushConstants.dataElementCount + x; // can no longer sum with | since there's no guarantees on row length
}

// Same numbers as forward FFT
uint64_t getChannelStartAddress(uint32_t channel)
{
	return pushConstants.rowMajorBufferAddress + channel * glsl::gl_NumWorkGroups().x * FFTParameters::TotalSize * sizeof(complex_t<scalar_t>);
}

// -------------------------------------------- FIRST AXIS IFFT ------------------------------------------------------------------
struct PreloadedFirstAxisAccessor : PreloadedAccessorMirrorTradeBase
{

	// Each column of the data currently stored in the rowMajorBuffer corresponds to (half) a column of the DFT of a column of the convolved image. With this in mind, knowing that the IFFT will yield
	// a real result, we can pack two consecutive columns as Z = C1 + iC2 and by linearity of DFT we get IFFT(C1) = Re(IFFT(Z)), IFFT(C2) = Im(IFFT(Z)). This is the inverse of the packing trick
	// in the forward FFT, with a much easier expression.
	// When we wrote the columns after the forward FFT, each thread wrote its even elements to the buffer. So it stands to reason that if we load the elements from each column in the same way, 
	// we can load each thread's even elements. Their odd elements, however, are the conjugates of even elements of some other threads - which element of which thread follows the same logic we used to 
	// unpack the FFT in the forward step.
	// Since complex conjugation is not linear, we cannot simply store two columns and pass around their conjugates. We load one, trade, then load the other, trade again.
	template<typename sharedmem_adaptor_t>
	void preload(uint32_t channel, sharedmem_adaptor_t adaptorForSharedMemory)
	{
		// Set LegacyBdaAccessor for reading
		rowMajorAccessor = LegacyBdaAccessor<complex_t<scalar_t> >::create(getChannelStartAddress(channel));
		// Load all even elements of first column
		for (uint32_t localElementIndex = 0; localElementIndex < (ElementsPerInvocation / 2); localElementIndex++)
		{
			const uint32_t index = WorkgroupSize * localElementIndex | workgroup::SubgroupContiguousIndex();
			preloaded[localElementIndex << 1] = rowMajorAccessor.get(rowMajorOffset(2 * glsl::gl_WorkGroupID().x, index));
		}
		// Get all odd elements by trading
		for (uint32_t localElementIndex = 1; localElementIndex < ElementsPerInvocation; localElementIndex += 2)
		{
			preloaded[localElementIndex] = conj(getDFTMirror<sharedmem_adaptor_t>(localElementIndex, adaptorForSharedMemory));
		}
		// Load even elements of second column, multiply them by i and add them to even positions
		// This makes even positions hold C1 + iC2
		for (uint32_t localElementIndex = 0; localElementIndex < (ElementsPerInvocation / 2); localElementIndex++)
		{
			const uint32_t index = WorkgroupSize * localElementIndex | workgroup::SubgroupContiguousIndex();
			preloaded[localElementIndex << 1] = preloaded[localElementIndex << 1] + rotateLeft<scalar_t>(rowMajorAccessor.get(rowMajorOffset(2 * glsl::gl_WorkGroupID().x + 1, index)));
		}
		// Finally, trade to get odd elements of second column. Note that by trading we receive an element of the form C1 + iC2 for an even position. The current odd position holds conj(C1) and we
		// want it to hold conj(C1) + i*conj(C2). So we first do conj(C1 + iC2) to yield conj(C1) - i*conj(C2). Then we subtract conj(C1) to get -i*conj(C2), negate that to get i * conj(C2), and finally
		// add conj(C1) back to have conj(C1) + i * conj(C2).
		for (uint32_t localElementIndex = 1; localElementIndex < ElementsPerInvocation; localElementIndex += 2)
		{
			// Thread 0's first odd element is Nyquist, which was packed alongside Zero - this means that what was said above breaks in this particular case and needs special treatment
			if (!workgroup::SubgroupContiguousIndex() && 1 == localElementIndex)
			{
				// preloaded[1] currently holds trash - this is because 0 and Nyquist are the only fixed points of T -> -T. 
				// preloaded[0] currently holds (C1(Z) - C2(N)) + i * (C1(N) + C2(Z)). This is because of how we loaded the even elements of both columns.
				// We want preloaded[0] to hold C1(Z) + i * C2(Z) and preloaded[1] to hold C1(N) + i * C2(N).
				// We can re-load C2(Z) + i * C2(N) and use it to unpack the values
				complex_t<scalar_t> c2 = rowMajorAccessor.get(rowMajorOffset(2 * glsl::gl_WorkGroupID().x + 1, 0));
				complex_t<scalar_t> p1 = { preloaded[0].imag() - c2.real(), c2.imag() };
				preloaded[1] = p1;
				complex_t<scalar_t> p0 = { preloaded[0].real() + c2.imag() , c2.real() };
				preloaded[0] = p0;
			}
			else
			{
				complex_t<scalar_t> otherThreadEven = conj(getDFTMirror<sharedmem_adaptor_t>(localElementIndex, adaptorForSharedMemory));
				otherThreadEven = otherThreadEven - preloaded[localElementIndex];
				otherThreadEven = otherThreadEven * scalar_t(-1);
				preloaded[localElementIndex] = preloaded[localElementIndex] + otherThreadEven;
			}
		}
	}

	void unload(uint32_t channel)
	{
		uint32_t2 imageDimensions;
		convolvedImage.GetDimensions(imageDimensions.x, imageDimensions.y);
		const uint32_t padding = uint32_t(TotalSize - imageDimensions.y) >> 1;
		for (uint32_t localElementIndex = 0; localElementIndex < ElementsPerInvocation; localElementIndex++)
		{
			const uint32_t index = WorkgroupSize * localElementIndex | workgroup::SubgroupContiguousIndex();
			const uint32_t paddedIndex = index - padding;
			if (paddedIndex >= 0 && paddedIndex < imageDimensions.y)
			{
				vector<scalar_t, 4> texValue = convolvedImage.Load(uint32_t2(2 * glsl::gl_WorkGroupID().x, paddedIndex));
				texValue[channel] = scalar_t(preloaded[localElementIndex].real());
				texValue.a = scalar_t(1);
				convolvedImage[uint32_t2(2 * glsl::gl_WorkGroupID().x, paddedIndex)] = texValue;

				texValue = convolvedImage.Load(uint32_t2(2 * glsl::gl_WorkGroupID().x + 1, paddedIndex));
				texValue[channel] = scalar_t(preloaded[localElementIndex].imag());
				texValue.a = scalar_t(1);
				convolvedImage[uint32_t2(2 * glsl::gl_WorkGroupID().x + 1, paddedIndex)] = texValue;
			}
		}
	}
	LegacyBdaAccessor<complex_t<scalar_t> > rowMajorAccessor;
};

[numthreads(FFTParameters::WorkgroupSize, 1, 1)]
void main(uint32_t3 ID : SV_DispatchThreadID)
{
	SharedMemoryAccessor sharedmemAccessor;
	// Set up the memory adaptor
	using sharedmem_adaptor_t = accessor_adaptors::StructureOfArrays<SharedMemoryAccessor, uint32_t, uint32_t, 1, FFTParameters::WorkgroupSize>;
	sharedmem_adaptor_t adaptorForSharedMemory;

	PreloadedFirstAxisAccessor preloadedAccessor;
	for (uint32_t channel = 0; channel < Channels; channel++)
	{
		adaptorForSharedMemory.accessor = sharedmemAccessor;
		preloadedAccessor.preload(channel, adaptorForSharedMemory);
		// Update state after preload
		sharedmemAccessor = adaptorForSharedMemory.accessor;
		workgroup::FFT<true, FFTParameters>::template __call(preloadedAccessor, sharedmemAccessor);
		preloadedAccessor.unload(channel);
	}
}
