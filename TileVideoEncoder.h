#ifndef _VIDEO_ENCODER
#define _VIDEO_ENCODER

#include <vector>

#include "../common/inc/NvHWEncoder.h"
#include "dynlink_nvcuvid.h" // <nvcuvid.h>

#define MAX_ENCODE_QUEUE 32

template<class T>
class BufferQueue {
    T** buffer;
    size_t size;
    size_t pending;
    unsigned int available_index;
    unsigned int pending_index;
public:
    BufferQueue() :
        buffer(NULL),
        size(0),
        pending(0),
        available_index(0),
        pending_index(0)
    { }

    ~BufferQueue()
    {
        delete[] buffer;
    }

    bool Initialize(T *items, unsigned int size)
    {
        this->size = size;
        pending = 0;
        available_index = 0;
        pending_index = 0;
        buffer = new T *[size];

        for (unsigned int i = 0; i < size; i++)
            buffer[i] = &items[i];

        return true;
    }

    T *GetAvailable()
    {
        if (pending == size)
            return NULL;

        T *item = NULL;
        item = buffer[available_index];
        available_index = (available_index + 1) % size;
        pending += 1;
        return item;
    }

    T *GetPending()
    {
        if (pending == 0)
            return NULL;

        T *item = buffer[pending_index];
        pending_index = (pending_index + 1) % size;
        pending -= 1;
        return item;
    }
};

typedef struct EncodeFrameConfig
{
    CUdeviceptr  device_pointer;
    unsigned int pitch;
    unsigned int width;
    unsigned int height;
} EncodeFrameConfig;

typedef struct TileEncodeContext
{
    CNvHWEncoder              hardwareEncoder;
    EncodeBuffer              encodeBuffer[MAX_ENCODE_QUEUE];
    BufferQueue<EncodeBuffer> encodeBufferQueue;
    size_t                    offsetX, offsetY;
} TileEncodeContext;

typedef struct TileDimensions
{
    const size_t rows;
    const size_t columns;
    const size_t count;
} TileDimensions;

class VideoEncoder
{
public:
    VideoEncoder(CUvideoctxlock lock, const unsigned int tileColumns, const unsigned int tileRows) :
        lock(lock),
        tileDimensions({tileRows, tileColumns, tileColumns * tileRows}),
        tileEncodeContext(tileDimensions.count),
        encodeBufferSize(0),
        framesEncoded(0)
        { assert(tileColumns > 0 && tileRows > 0); }
    virtual ~VideoEncoder()
        { }

    NVENCSTATUS Initialize(void*, const NV_ENC_DEVICE_TYPE);
    NVENCSTATUS CreateEncoders(EncodeConfig&);
    NVENCSTATUS Deinitialize();
    NVENCSTATUS EncodeFrame(
        EncodeFrameConfig*, const NV_ENC_PIC_STRUCT type = NV_ENC_PIC_STRUCT_FRAME, const bool flush = false);
    NVENCSTATUS AllocateIOBuffers(const EncodeConfig*);
    size_t      GetEncodedFrames() const { return framesEncoded; }
    GUID        GetPresetGUID()  const { return presetGUID; }

protected:
    GUID                           presetGUID;
    TileDimensions                 tileDimensions;
    std::vector<TileEncodeContext> tileEncodeContext;
    CUvideoctxlock                 lock;

    size_t                         encodeBufferSize;
    size_t                         framesEncoded;

private:
    NVENCSTATUS AllocateIOBuffer(TileEncodeContext&, const EncodeConfig&);
    NVENCSTATUS ReleaseIOBuffers();
    NVENCSTATUS FlushEncoder();
};

#endif

