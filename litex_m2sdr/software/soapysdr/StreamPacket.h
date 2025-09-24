#ifndef LIME_STREAMSPACKET_H
#define LIME_STREAMSPACKET_H

#include <cassert>
#include <cstring>

class PacketMeta
{
  public:
    uint64_t timestamp;
    bool useTimestamp{ 0 }; ///< Whether to use the timestamp or not.
    bool flush{ 0 }; ///< Whether to flush the whole packet early or not.
};

/**
  @brief Class for holding a multiple RF samples buffers with timestamp
 */
class PacketData
{
  private:
    PacketData() = delete;

  public:
    PacketData(uint32_t samplesCapacity, uint32_t channelCount, uint32_t sampleSize)
        : mCapacity(samplesCapacity)
        , frameSize(sampleSize)
        , mChannelCount(channelCount)
    {
        assert(channelCount > 0);
        assert(channelCount <= MAX_CHANNEL_COUNT);

        for (uint32_t i = 0; i < mChannelCount; ++i)
            memset(channel, 0, sizeof(channel));
        for (int i = 0; i < mChannelCount; ++i)
        {
            head[i] = tail[i] = channel[i] = static_cast<uint8_t*>(malloc(samplesCapacity * sampleSize));
            memset(channel[i], 0, samplesCapacity * sampleSize);
        }
    }

    ~PacketData()
    {
        for (int i = 0; i < mChannelCount; ++i)
            free(channel[i]);
    }

    /// @return The amount of samples remaining.
    inline constexpr uint32_t size() const { return (tail[0] - head[0]) / frameSize; };

    /**
      @brief Gets whether the packet is empty or not.
      @return True if empty.
      @return false if it has elements.
     */
    inline constexpr bool empty() const { return size() == 0; };

    /**
      @brief Gets the maximum amount of samples this packet can hold.
      @return The amount of samples the packet can hold.
     */
    inline constexpr int capacity() const { return mCapacity; };

    /**
      @brief Gets whether the packet is full or not.
      @return True if full.
      @return False if there is still space..
     */
    inline constexpr bool isFull() const { return (mCapacity - (tail[0] - channel[0]) / frameSize) < 1; };

    /**
      @brief Gets the channel count of this packet.
      @return The amount of channels this packet holds the information for.
     */
    inline constexpr int channelCount() const { return mChannelCount; };

    /**
      @brief Copies samples data to buffer.
      @tparam T The type of samples to copy.
      @param src The source array of the samples.
      @param count The amount of samples to copy.
      @return Actual copied samples count
     */
    template<class T> inline int push(const T* const* src, uint32_t count)
    {
        static_assert(std::is_trivially_copyable_v<T> == true);
        assert(sizeof(T) == frameSize);

        const uint32_t freeSamples = mCapacity - (tail[0] - channel[0]) / frameSize;
        const uint32_t samplesToCopy = std::min(freeSamples, count);
        for (uint8_t i = 0; i < mChannelCount; ++i)
        {
            assert(src[i] != nullptr);
            std::memcpy(tail[i], src[i], samplesToCopy * frameSize);
            tail[i] += samplesToCopy * frameSize;
        }
        return samplesToCopy;
    }

    /**
      @brief Removes a given amount of samples from the packet.
      @param count The number of samples to remove.
      @return Number of samples actually removed.
     */
    inline uint32_t pop(uint32_t count)
    {
        const uint32_t toPop = std::min(count, (static_cast<uint32_t>(tail[0] - head[0]) / frameSize));
        for (uint8_t i = 0; i < mChannelCount; ++i)
            head[i] += toPop * frameSize;
        return toPop;
    }

    /**
      @brief Gets a pointer to the front element of the packet.
      @return A pointer to the front of the packet elements.
     */
    inline void* const* front() const { return reinterpret_cast<void* const*>(head); }

    /**
      @brief Gets a pointer to the back element of the packet.
      @return A pointer to the back of the packet elements.
     */
    inline void* const* back() const { return reinterpret_cast<void* const*>(tail); }

    /** @brief Resets all the packet to be blank. */
    inline void Reset()
    {
        for (uint8_t i = 0; i < mChannelCount; ++i)
            tail[i] = head[i] = channel[i];
    }

    /**
      @brief Sets the amount of samples of this packet.
      @param sampleCount The amount of samples this packet has.
     */
    inline void SetSize(uint32_t sampleCount)
    {
        for (uint8_t i = 0; i < mChannelCount; ++i)
            tail[i] = channel[i] + sampleCount * frameSize;
    }

    /**
      @brief Scales the packet's samples' I and Q values by the given multiplier.
      @tparam T The type of samples the packet is holding.
      @param iScale The multiplier with which to multiply all the I values.
      @param qScale The multiplier with which to multiply all the Q values.
     */
    template<class T> void Scale(float iScale, float qScale)
    {
        int samplesCount = size();
        for (int c = 0; c < mChannelCount; ++c)
        {
            T* samples = reinterpret_cast<T*>(head[c]);
            for (int i = 0; i < samplesCount; ++i)
            {
                samples[i].real(samples[i].real() * iScale);
                samples[i].imag(samples[i].imag() * qScale);
            }
        }
    }

  private:
    static constexpr uint8_t MAX_CHANNEL_COUNT = 2;

    uint8_t* head[MAX_CHANNEL_COUNT];
    uint8_t* tail[MAX_CHANNEL_COUNT];
    uint8_t* channel[MAX_CHANNEL_COUNT];

    uint32_t mCapacity;
    uint8_t frameSize;
    uint8_t mChannelCount;
};

class StreamPacket
{
  private:
    StreamPacket() = delete;

  public:
    StreamPacket(uint32_t samplesCapacity, uint32_t channelCount, uint32_t sampleSize)
        : samples(samplesCapacity, channelCount, sampleSize)
    {
    }

    void Reset()
    {
        meta.timestamp = 0;
        meta.flush = false;
        samples.Reset();
    }

    PacketMeta meta;
    PacketData samples;
};

#endif /* LIME_STREAMSPACKET_H */