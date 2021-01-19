#include <text/NumberText.h>
#include <text/TextFunctions.h>
#include <text/UTF.h>
#include <bmp/BMP.h>
#include <Array.h>
#include <ArrayDef.h>
#include <File.h>
#include <Types.h>

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#undef DeleteFile

#include <assert.h>
#include <comdef.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <utility>

#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

using namespace OUTER_NAMESPACE;
using namespace OUTER_NAMESPACE :: COMMON_LIBRARY_NAMESPACE;

bool hresultSuccess(HRESULT h) {
    if (SUCCEEDED(h)) {
        return true;
    }

    IErrorInfo* pErrorInfo = nullptr;
    if (!SUCCEEDED(GetErrorInfo(0,&pErrorInfo))) {
        pErrorInfo = nullptr;
    }

    _com_error error(h, pErrorInfo);
    printf("ERROR: %ls\n", error.ErrorMessage());
    fflush(stdout);

    return false;
}

template<typename T>
struct ReleasePtr {
    T* p;

    ReleasePtr() noexcept : p(nullptr) {}
    ~ReleasePtr() noexcept {
        if (p != nullptr) {
            p->Release();
        }
    }

    // No copying (though if needed, AddRef would work)
    ReleasePtr(const ReleasePtr&) = delete;
    ReleasePtr& operator=(const ReleasePtr&) = delete;

    // Moving allowed
    ReleasePtr(ReleasePtr&& other) noexcept : p(other.p) {
        other.p = nullptr;
    }
    ReleasePtr& operator=(ReleasePtr&& other) noexcept {
        if (p == other.p) {
            return *this;
        }
        if (p != nullptr) {
            p->Release();
        }
        p = other.p;
        other.p = nullptr;
        return *this;
    }

    // Manual release
    void release() noexcept {
        if (p != nullptr) {
            p->Release();
        }
        p = nullptr;
    }

    T* operator->() noexcept {
        return p;
    }
};

struct FormatInfo {
    uint32 width;
    uint32 height;
    uint32 fpsNumerator = 30;
    uint32 fpsDenominator = 1;
    uint32 averageBitsPerSecond = 4500000; // 4500kbps default
    GUID imageFormat = MFVideoFormat_RGB32;
    GUID videoFormat = MFVideoFormat_H264;
};

// 100ns units, so 10 million of them per second
constexpr static uint64 timeUnitsPerSecond = 10000000;

static std::pair<ReleasePtr<IMFSinkWriter>, DWORD> createWriter(const char* filename, const FormatInfo& format)
{
    std::pair<ReleasePtr<IMFSinkWriter>, DWORD> retVal{ReleasePtr<IMFSinkWriter>(),0};

    // Convert filename from UTF8 to UTF16
    const size_t utf8Length = text::stringSize(filename);
    const size_t utf16Length = text::UTF16Length(filename, utf8Length);
    Array<uint16> utf16Filename;
    utf16Filename.setSize(utf16Length+1);
    text::UTF8ToUTF16(filename, utf8Length, utf16Filename.data());
    utf16Filename.last() = 0;

    ReleasePtr<IMFSinkWriter> writer;
    if (!hresultSuccess(MFCreateSinkWriterFromURL((LPCWSTR)utf16Filename.data(),nullptr,nullptr,&writer.p))) {
        return retVal;
    }

    // Set the output media type.
    ReleasePtr<IMFMediaType> outputMediaType;
    if (!hresultSuccess(MFCreateMediaType(&outputMediaType.p))) {
        return retVal;
    }
    if (!hresultSuccess(outputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        !hresultSuccess(outputMediaType->SetGUID(MF_MT_SUBTYPE, format.videoFormat)) ||
        !hresultSuccess(outputMediaType->SetUINT32(MF_MT_AVG_BITRATE, format.averageBitsPerSecond)) ||
        !hresultSuccess(outputMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        !hresultSuccess(MFSetAttributeSize(outputMediaType.p, MF_MT_FRAME_SIZE, format.width, format.height)) ||
        !hresultSuccess(MFSetAttributeRatio(outputMediaType.p, MF_MT_FRAME_RATE, format.fpsNumerator, format.fpsDenominator)) ||
        !hresultSuccess(MFSetAttributeRatio(outputMediaType.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1))
    ) {
        return retVal;
    }
    DWORD streamIndex;
    if (!hresultSuccess(writer->AddStream(outputMediaType.p, &streamIndex))) {
        return retVal;
    }

    // Set the input media type.
    ReleasePtr<IMFMediaType> inputMediaIndex;
    if (!hresultSuccess(MFCreateMediaType(&inputMediaIndex.p))) {
        return retVal;
    }
    if (!hresultSuccess(inputMediaIndex->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        !hresultSuccess(inputMediaIndex->SetGUID(MF_MT_SUBTYPE, format.imageFormat)) ||
        !hresultSuccess(inputMediaIndex->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
        !hresultSuccess(MFSetAttributeSize(inputMediaIndex.p, MF_MT_FRAME_SIZE, format.width, format.height)) ||
        !hresultSuccess(MFSetAttributeRatio(inputMediaIndex.p, MF_MT_FRAME_RATE, format.fpsNumerator, format.fpsDenominator)) ||
        !hresultSuccess(MFSetAttributeRatio(inputMediaIndex.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1))
    ) {
        return retVal;
    }
    if (!hresultSuccess(writer->SetInputMediaType(streamIndex, inputMediaIndex.p, nullptr))) {
        return retVal;
    }

    // Tell the sink writer to start accepting data.
    if (!hresultSuccess(writer->BeginWriting())) {
        return retVal;
    }

    // Return the pointer to the caller.
    retVal.first = std::move(writer);
    retVal.second = streamIndex;

    return retVal;
}

static bool writeFrame(
    IMFSinkWriter *writer,
    DWORD streamIndex,
    const uint32* imageData,
    const uint64 frameStartTime,
    const uint64 frameEndTime,
    const FormatInfo& format
) {
    const LONG scanlineBytes = sizeof(uint32) * format.width;
    const DWORD bufferSizeInBytes = scanlineBytes * format.height;

    // TODO: Can we reuse the sample and the buffer from frame to frame?
    ReleasePtr<IMFMediaBuffer> buffer;

    // Create a new memory buffer.
    if (!hresultSuccess(MFCreateMemoryBuffer(bufferSizeInBytes,&buffer.p))) {
        return false;
    }

    // Lock the buffer and copy the video frame to the buffer.
    BYTE *bufferData = nullptr;
    if (!hresultSuccess(buffer->Lock(&bufferData, nullptr, nullptr))) {
        return false;
    }
    assert(bufferData != nullptr);
    // TODO: If images with odd widths are ever supported, this might be able to be adjusted to skip the last pixel in each row.
    HRESULT hr = MFCopyImage(
        bufferData,
        scanlineBytes,              // Destination scanline stride
        (const BYTE*)imageData,
        scanlineBytes,              // Source scanline stride
        scanlineBytes,              // Scanline data size in bytes
        format.height
    );
    // Unlock buffer regardless of success
    buffer->Unlock();

    if (!hresultSuccess(hr)) {
        return false;
    }

    // Set the data length of the buffer.
    if (!hresultSuccess(buffer->SetCurrentLength(bufferSizeInBytes))) {
        return false;
    }

    // Create a media sample and add the buffer to the sample.
    ReleasePtr<IMFSample> pSample;
    if (!hresultSuccess(MFCreateSample(&pSample.p))) {
        return false;
    }
    if (!hresultSuccess(pSample->AddBuffer(buffer.p))) {
        return false;
    }

    // Set the time stamp and the duration.
    if (!hresultSuccess(pSample->SetSampleTime((LONGLONG)frameStartTime))) {
        return false;
    }
    if (!hresultSuccess(pSample->SetSampleDuration((LONGLONG)(frameEndTime - frameStartTime)))) {
        return false;
    }

    // Send the sample to the Sink Writer.
    if (!hresultSuccess(writer->WriteSample(streamIndex, pSample.p))) {
        return false;
    }

    return true;
}

struct CoUninitializer {
    ~CoUninitializer() {
        CoUninitialize();
    }
};

struct MFShutdowner {
    ~MFShutdowner() {
        MFShutdown();
    }
};

bool parseResolutionText(const char* res, uint32& width, uint32& height) {
    size_t resLength = text::stringSize(res);
    const char* resEnd = res + resLength;
    size_t widthLength = text::findFirstCharacter(res, resEnd, 'x') - res;
    if (widthLength >= resLength-1) {
        return false;
    }
    const char* widthEnd = res + widthLength;
    const char* heightBegin = widthEnd + 1;
    size_t numCharsUsed = text::textToInteger(res, widthEnd, width);
    if (numCharsUsed != widthEnd-res) {
        return false;
    }
    numCharsUsed = text::textToInteger(heightBegin, resEnd, width);
    if (numCharsUsed != resEnd-heightBegin) {
        return false;
    }
    return true;
}

// NOTE: The filename will not be zero-terminated!
bool getNextFilename(Array<char>& filename) {
    bool endOfFileList = false;
    bool foundFilename = false;
    while (!foundFilename) {
        // Skip any blank lines
        char c;
        do {
            size_t bytesRead = fread(&c, sizeof(char), 1, stdin);
            if (bytesRead == 0) {
                return false;
            }
        } while (c == '\n' || c == '\r');

        do {
            filename.append(c);
            size_t bytesRead = fread(&c, sizeof(char), 1, stdin);
            if (bytesRead == 0) {
                endOfFileList = true;
                break;
            }
        } while (c != '\n' && c != '\r');

        // Skip lines starting with #, so that it's easy to comment out lines.
        if (filename[0] == '#') {
            filename.setSize(0);
            if (endOfFileList) {
                return false;
            }
            continue;
        }
        foundFilename = true;
    }

    if (filename.size() == 4 && (
        text::areEqualSizeStringsEqual(filename.data(), "stop", 4) ||
        text::areEqualSizeStringsEqual(filename.data(), "quit", 4) ||
        text::areEqualSizeStringsEqual(filename.data(), "exit", 4) ||
        text::areEqualSizeStringsEqual(filename.data(), "done", 4))
    ) {
        filename.setSize(0);
        return false;
    }

    if (filename.size() == 3 && (
        text::areEqualSizeStringsEqual(filename.data(), "end", 3))
    ) {
        filename.setSize(0);
        return false;
    }

    return !endOfFileList;
}

// Example command line:
// VideoIO.exe 1920x1080 outputVideo.mp4 < imageFilenames.txt
// If at least the first image is a bitmap file with the correct width and height:
// VideoIO.exe outputVideo.mp4 < imageFilenames.txt
// If the output filename extension is .wmv, it will be encoded using the WMV3 codec,
// instead of the H.264 codec.
//
// Special "filenames"
// - If any of the filename lines are "delete", the previous image will be deleted.
// - Any lines starting with # will be skipped, for easy commenting-out of files.
// - If any of the filename lines are "stop", "quit", "done", "exit", or "end",
//   processing will be stopped.
// - If any of the filename lines are "repeat <number>", the previous
//   image will be included <number>-1 additional times.
//
// NOTE: H.264 codec does not support odd width or height!
int main(int argc, char** argv)
{
    if (!hresultSuccess(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED))) {
        printf("ERROR: Failed to initialize COM.  Exiting.\n");
        fflush(stdout);
        return -1;
    }
    CoUninitializer couninit;

    if (!hresultSuccess(MFStartup(MF_VERSION))) {
        printf("ERROR: Failed to start Media Foundation.  Exiting.\n");
        fflush(stdout);
        return -1;
    }
    MFShutdowner mfshutdown;

    if (argc < 2) {
        printf("ERROR: Not enough command line arguments.  Exiting.\n");
        fflush(stdout);
        return -1;
    }

    uint32 width;
    uint32 height;
    bool hasSpecifiedResolution = parseResolutionText(argv[1], width, height);
    if (hasSpecifiedResolution && argc != 3) {
        printf("ERROR: Not enough command line arguments.  Exiting.\n");
        fflush(stdout);
        return -1;
    }

    const char* outputFilename = argv[hasSpecifiedResolution ? 2 : 1];
    size_t outputFilenameLength = text::stringSize(outputFilename);
    bool isWMVOutput = false;
    if (outputFilenameLength >= 5 &&
        text::areEqualSizeStringsEqual(outputFilename+outputFilenameLength-4,".wmv",4)
    ) {
        isWMVOutput = true;
    }

    Array<uint32> imageData;

    size_t pixelCount = 0;
    if (hasSpecifiedResolution) {
        if ((width & 1) || (height & 1)) {
            printf("ERROR: H.264 codec does not support odd width or height.  Exiting.\n");
            fflush(stdout);
            return -1;
        }

        pixelCount = size_t(width)*height;
        if (pixelCount == 0) {
            printf("ERROR: Either width or height is zero in %ux%u resolution.  Exiting.\n", width, height);
            fflush(stdout);
            return -1;
        }
        // Even a 7680x4320 (8K) image buffer is less than 128 MB, so 1GB should be a plenty large safety threshold.
        if (pixelCount * sizeof(uint32) > 1024*1024*1024) {
            printf("ERROR: %ux%u resolution would result in a very large image buffer.  Exiting.\n", width, height);
            fflush(stdout);
            return -1;
        }

        imageData.setCapacity(pixelCount);
    }

    FormatInfo format{0,0};
    format.videoFormat = isWMVOutput ? MFVideoFormat_WMV3 : MFVideoFormat_H264;
    // TODO: Support other format info!

    std::pair<ReleasePtr<IMFSinkWriter>, DWORD> writerAndStreamIndex;

    // Current frame start time in 100ns units.
    uint64 frameStartTime = 0;

    Array<char> previousFilename;

    // Send frames to the sink writer.
    size_t framei = 0;
    while (true) {
        // Read input frame filename from stdin.
        Array<char> inputFilename;
        // Skip any blank lines
        bool fileListContinues = getNextFilename(inputFilename);
        if (!fileListContinues && inputFilename.size() == 0) {
            break;
        }

        if (inputFilename.size() == 6 && text::areEqualSizeStringsEqual(inputFilename.data(),"delete",6)) {
            if (previousFilename.size() != 0) {
                DeleteFile(previousFilename.data());
                previousFilename.setSize(0);
            }
            inputFilename.setSize(0);
            continue;
        }
        if (previousFilename.size() != 0 && inputFilename.size() > 7 && text::areEqualSizeStringsEqual(inputFilename.data(),"repeat ",7)) {
            const char* numberText = inputFilename.data() + 7;
            const char* numberTextEnd = inputFilename.end();
            size_t numRepeats;
            size_t charactersUsed = text::textToInteger(numberText, numberTextEnd, numRepeats);
            if (charactersUsed == numberTextEnd-numberText) {
                // NOTE: The image was already included once, so skip the first one here.
                for (size_t repeat = 1; repeat < numRepeats; ++repeat) {
                    uint64_t frameEndTime = (timeUnitsPerSecond * framei * format.fpsDenominator) / format.fpsNumerator;
                    if (!writeFrame(writerAndStreamIndex.first.p, writerAndStreamIndex.second, imageData.data(), frameStartTime, frameEndTime, format)) {
                        return -1;
                    }
                    ++framei;
                    frameStartTime = frameEndTime;
                }
                if (!fileListContinues) {
                    break;
                }
                continue;
            }
        }

        bool isBitmapFile = false;
        if (inputFilename.size() >= 5 &&
            text::areEqualSizeStringsEqual(inputFilename.end()-4,".bmp",4)
        ) {
            isBitmapFile = true;
        }

        // Add terminating zero.
        inputFilename.append(0);

        if (!hasSpecifiedResolution && !isBitmapFile) {
            // If the resolution isn't specified on the command line,
            // it needs to be retrieved from the bitmap file.
            printf("ERROR: No resolution specified and \"%s\" is not a bitmap file, so cannot deduce the resolution.  Exiting.\n", inputFilename.data());
            fflush(stdout);
            return -1;
        }

        // Get image data if different image from previous frame.
        if (previousFilename.size() != inputFilename.size() ||
            !text::areEqualSizeStringsEqual(previousFilename.data(), inputFilename.data(), inputFilename.size())
        ) {
            if (isBitmapFile) {
                size_t bmpWidth;
                size_t bmpHeight;
                bool hasAlpha;
                bool success = bmp::ReadBMPFile(inputFilename.data(), imageData, bmpWidth, bmpHeight, hasAlpha);
                if (!success) {
                    printf("ERROR: Unable to read bitmap file \"%s\".  Exiting.\n", inputFilename.data());
                    fflush(stdout);
                    return -1;
                }
                if (hasSpecifiedResolution) {
                    if (bmpWidth != width || bmpHeight != height) {
                        return -1;
                    }
                }
                else {
                    width = uint32(bmpWidth);
                    height = uint32(bmpHeight);
                    if ((width & 1) || (height & 1)) {
                        printf("ERROR: H.264 codec does not support odd width or height.  Exiting.\n");
                        fflush(stdout);
                        return -1;
                    }

                    pixelCount = size_t(width)*height;
                    if (pixelCount == 0) {
                        printf("ERROR: Either width or height is zero in %ux%u resolution.  Exiting.\n", width, height);
                        fflush(stdout);
                        return -1;
                    }
                    hasSpecifiedResolution = true;
                }
            }
            else {
                ReadFileHandle handle = OpenFileRead(inputFilename.data());
                if (handle) {
                    printf("ERROR: Unable to open non-bitmap file \"%s\".  Exiting.\n", inputFilename.data());
                    fflush(stdout);
                    return -1;
                }
                uint64 fileSize = GetFileSize(handle);
                if (fileSize != sizeof(uint32)*pixelCount) {
                    printf("ERROR: Non-bitmap file \"%s\" must have size %zu, but has size %zu.  Exiting.\n", inputFilename.data(), sizeof(uint32)*pixelCount, size_t(fileSize));
                    fflush(stdout);
                    return -1;
                }
                imageData.setSize(pixelCount);
                size_t numBytesRead = ReadFile(handle, imageData.data(), fileSize);
                if (numBytesRead != fileSize) {
                    printf("ERROR: Unable to read %zu bytes from non-bitmap file \"%s\".  Exiting.\n", size_t(fileSize), inputFilename.data());
                    fflush(stdout);
                    return -1;
                }
            }
        }

        // Now that we're guaranteed to have a width and height, we can make the writer.
        if (framei == 0) {
            format.width = width;
            format.height = height;
            writerAndStreamIndex = createWriter(outputFilename, format);
            if (writerAndStreamIndex.first.p == nullptr) {
                printf("ERROR: Unable to create video writer for \"%s\" with %ux%u resolution.  Exiting.\n", outputFilename, width, height);
                fflush(stdout);
                return -1;
            }
        }

        uint64_t frameEndTime = (timeUnitsPerSecond * framei * format.fpsDenominator) / format.fpsNumerator;
        if (!writeFrame(writerAndStreamIndex.first.p, writerAndStreamIndex.second, imageData.data(), frameStartTime, frameEndTime, format)) {
            printf("ERROR: Failed to write frame %zu of \"%s\".  Exiting.\n", framei, outputFilename);
            fflush(stdout);
            return -1;
        }
        if (!fileListContinues) {
            break;
        }

        ++framei;
        frameStartTime = frameEndTime;
        previousFilename = std::move(inputFilename);
    }

    if (!hresultSuccess(writerAndStreamIndex.first->Finalize())) {
        printf("ERROR: Failed to finalize \"%s\".  Exiting.\n", outputFilename);
        fflush(stdout);
        return -1;
    }

    return 0;
}
