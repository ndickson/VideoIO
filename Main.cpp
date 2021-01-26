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

// NOTE: The filename will not be zero-terminated!
bool getNextFilename(Array<char>& filename) {
    filename.setSize(0);
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
// VideoIO.exe < imageFilenames.txt
// If at least the first image is a bitmap file with the correct width and height:
// VideoIO.exe outputVideo.mp4 < imageFilenames.txt
// If the output filename extension is .wmv, it will be encoded using the WMV3 codec,
// instead of the H.264 codec.
//
// Special "filenames":
// - "stop", "quit", "done", "exit", or "end": Processing will be stopped.
// - "cancel": Processing will be stopped, and the output file will be deleted.
// - "delete": The previous image will be deleted.
// - "repeat <number>": The previous image will be included <number>-1 additional times, for a total of <number>.
// - "resolution <number>x<number>": Sets the resolution, if no images have been encountered yet.
// - "fps <number>" or "fps <number>/<number>": Sets the frames per second, possibly as a fraction, if no images have been encountered yet.
// - "bitrate <number>": Sets the target average bits per second, if no images have been encountered yet.
// - "output <filename>": Specifies the output filename.
// - "image <filename>": In case a filename might need to match one of the commands above, this gives a way to be explicit about the filename.
// - Any lines starting with # will be skipped, for easy commenting-out of files.
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

    Array<uint32> imageData;

    size_t pixelCount = 0;

    FormatInfo format{0,0};

    std::pair<ReleasePtr<IMFSinkWriter>, DWORD> writerAndStreamIndex;

    // Current frame start time in 100ns units.
    uint64 frameStartTime = 0;

    Array<char> previousFilename;
    Array<char> inputFilename;
    Array<char> outputFilename;

    // Send frames to the sink writer.
    size_t framei = 0;
    bool fileListContinues = true;
    bool cancelled = false;
    while (fileListContinues) {
        // Read input frame filename from stdin.
        fileListContinues = getNextFilename(inputFilename);
        if (inputFilename.size() == 0) {
            continue;
        }

        if (inputFilename.size() == 6 && text::areEqualSizeStringsEqual(inputFilename.data(),"cancel",6)) {
            cancelled = true;
            printf("NOTE: Cancelling video encoding.\n");
            fflush(stdout);
            break;
        }

        // "delete" command
        if (inputFilename.size() == 6 && text::areEqualSizeStringsEqual(inputFilename.data(),"delete",6)) {
            if (previousFilename.size() != 0) {
                DeleteFile(previousFilename.data());
                previousFilename.setSize(0);
                imageData.setSize(0);
            }
            else {
                printf("WARNING: Invalid \"delete\" command: no previous file to delete.\n");
                fflush(stdout);
            }
            continue;
        }

        // "repeat <number>" command
        if (inputFilename.size() > 7 && text::areEqualSizeStringsEqual(inputFilename.data(),"repeat ",7)) {
            const char* numberText = inputFilename.data() + 7;
            const char* numberTextEnd = inputFilename.end();
            size_t numRepeats;
            size_t charactersUsed = text::textToInteger(numberText, numberTextEnd, numRepeats);
            if (charactersUsed == numberTextEnd-numberText && imageData.size() != 0) {
                // NOTE: The image was already included once, so skip the first one here.
                for (size_t repeat = 1; repeat < numRepeats; ++repeat) {
                    uint64_t frameEndTime = (timeUnitsPerSecond * framei * format.fpsDenominator) / format.fpsNumerator;
                    if (!writeFrame(writerAndStreamIndex.first.p, writerAndStreamIndex.second, imageData.data(), frameStartTime, frameEndTime, format)) {
                        return -1;
                    }
                    ++framei;
                    frameStartTime = frameEndTime;
                }
            }
            else {
                printf("WARNING: Invalid \"repeat <number>\" command: either no previous file to repeat or invalid number of repeats.\n");
                fflush(stdout);
            }
            continue;
        }

        // "fps <number>" or "fps <number>/<number>" command
        if (inputFilename.size() > 4 && text::areEqualSizeStringsEqual(inputFilename.data(),"fps ",4)) {
            const char* numberText = inputFilename.data() + 4;
            const char* numberTextEnd = inputFilename.end();
            uint32 numerator;
            uint32 denominator = 1;
            size_t charactersUsed = text::textToInteger(numberText, numberTextEnd, numerator);
            if (charactersUsed != numberTextEnd-numberText && numberText[charactersUsed] == '/') {
                numberText += charactersUsed+1;
                charactersUsed = text::textToInteger(numberText, numberTextEnd, denominator);
            }
            if (charactersUsed == numberTextEnd-numberText && framei == 0 && denominator != 0 && numerator != 0 && numerator < timeUnitsPerSecond*denominator) {
                format.fpsNumerator = numerator;
                format.fpsDenominator = denominator;
            }
            else {
                printf("WARNING: Invalid \"fps <number>[/<number>]\" command: either invalid integer or fraction, or video already started.\n");
                fflush(stdout);
            }
            continue;
        }

        // "bitrate <number>" command
        if (inputFilename.size() > 8 && text::areEqualSizeStringsEqual(inputFilename.data(),"bitrate ",8)) {
            const char* numberText = inputFilename.data() + 8;
            const char* numberTextEnd = inputFilename.end();
            uint32 bitRate;
            size_t charactersUsed = text::textToInteger(numberText, numberTextEnd, bitRate);
            if (charactersUsed == numberTextEnd-numberText && framei == 0 && bitRate != 0) {
                format.averageBitsPerSecond = bitRate;
            }
            else {
                printf("WARNING: Invalid \"bitrate <number>\" command: either invalid integer, or video already started.\n");
                fflush(stdout);
            }
            continue;
        }

        // "resolution <number>x<number>" command
        if (inputFilename.size() > 11 && text::areEqualSizeStringsEqual(inputFilename.data(),"resolution ",11)) {
            const char* numberText = inputFilename.data() + 11;
            const char* numberTextEnd = inputFilename.end();
            uint32 width = 0;
            uint32 height = 0;
            size_t charactersUsed = text::textToInteger(numberText, numberTextEnd, width);
            if (charactersUsed != numberTextEnd-numberText && numberText[charactersUsed] == 'x') {
                numberText += charactersUsed+1;
                charactersUsed = text::textToInteger(numberText, numberTextEnd, height);
            }
            if ((width & 1) || (height & 1)) {
                printf("ERROR: H.264 codec does not support odd width or height.  Exiting.\n");
                fflush(stdout);
                return -1;
            }
            if (charactersUsed == numberTextEnd-numberText && framei == 0 && width != 0 && height != 0) {
                format.width = width;
                format.height = height;
                pixelCount = size_t(width) * height;
            }
            else {
                printf("WARNING: Invalid \"resolution <number>x<number>\" command: either invalid integers, or video already started.\n");
                fflush(stdout);
            }
            continue;
        }

        if (inputFilename.size() > 7 && text::areEqualSizeStringsEqual(inputFilename.data(),"output ",7)) {
            // Remove the first 7 characters, i.e. "output ".
            outputFilename.setSize(inputFilename.size() - 7 + 1);
            for (size_t i = 7, n = inputFilename.size(); i < n; ++i) {
                outputFilename[i-7] = inputFilename[i];
            }
            outputFilename.last() = 0;
            bool isWMVOutput = false;
            if (outputFilename.size() >= 6 &&
                text::areEqualSizeStringsEqual(outputFilename.end()-5,".wmv",4)
            ) {
                isWMVOutput = true;
            }
            format.videoFormat = isWMVOutput ? MFVideoFormat_WMV3 : MFVideoFormat_H264;
            continue;
        }

        bool commandStartedWithImage = false;
        bool commandStartedWithPipe = false;
        if (inputFilename.size() > 6 && text::areEqualSizeStringsEqual(inputFilename.data(),"image ",6)) {
            // Remove the first 6 characters, i.e. "image ".
            for (size_t i = 6, n = inputFilename.size(); i < n; ++i) {
                inputFilename[i-6] = inputFilename[i];
            }
            inputFilename.setSize(inputFilename.size()-6);
            commandStartedWithImage = true;
        }
        else if (inputFilename.size() > 5 && text::areEqualSizeStringsEqual(inputFilename.data(),"pipe ",5)) {
            commandStartedWithPipe = true;
        }

        bool isBitmapFile = false;
        if (!commandStartedWithPipe && inputFilename.size() >= 5 &&
            text::areEqualSizeStringsEqual(inputFilename.end()-4,".bmp",4)
        ) {
            isBitmapFile = true;
        }

        if ((format.width == 0 || format.height == 0) && !isBitmapFile) {
            // If the resolution isn't specified on the command line,
            // it needs to be retrieved from the bitmap file.
            printf("ERROR: No resolution specified and \"%s\" is not a bitmap file, so cannot deduce the resolution.  Exiting.\n", inputFilename.data());
            fflush(stdout);
            return -1;
        }

        if (commandStartedWithPipe) {
            uintptr_t pipeReadHandleNumber;
            size_t numCharactersUsed = text::textToInteger<16>(inputFilename.data() + 5, inputFilename.end(), pipeReadHandleNumber);
            if (numCharactersUsed != inputFilename.size()-5) {
                printf("ERROR: Invalid pipe \"%s\" specified.  Exiting.\n", inputFilename.data()+5);
                fflush(stdout);
                return -1;
            }
            const HANDLE pipeReadHandle = (HANDLE)pipeReadHandleNumber;

            imageData.setSize(pixelCount);
            // NOTE: This supports at most 4GB of data, but that's more than our size limit anyway, so it should be fine.
            DWORD numBytesRead;
            BOOL success = ::ReadFile(pipeReadHandle, imageData.data(), (DWORD)(sizeof(uint32)*pixelCount), &numBytesRead, nullptr);
            if (!success) {
                printf("ERROR: Unable to read pipe \"%s\".  Exiting.\n", inputFilename.data()+5);
                fflush(stdout);
                return -1;
            }

            inputFilename.setSize(0);
            previousFilename.setSize(0);
        }
        else {
            // Add terminating zero.
            inputFilename.append(0);

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
                    if (format.width != 0 && format.height != 0) {
                        if (bmpWidth != format.width || bmpHeight != format.height) {
                            return -1;
                        }
                    }
                    else {
                        format.width = uint32(bmpWidth);
                        format.height = uint32(bmpHeight);
                        if ((format.width & 1) || (format.height & 1)) {
                            printf("ERROR: H.264 codec does not support odd width or height.  Exiting.\n");
                            fflush(stdout);
                            return -1;
                        }

                        pixelCount = size_t(format.width)*format.height;
                        if (pixelCount == 0) {
                            printf("ERROR: Either width or height is zero in %ux%u resolution.  Exiting.\n", format.width, format.height);
                            fflush(stdout);
                            return -1;
                        }
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
        }

        // Now that we're guaranteed to have a width and height, we can make the writer.
        if (framei == 0) {
            if (outputFilename.size() == 0) {
                printf("ERROR: No output filename specified.  Exiting.\n");
                fflush(stdout);
                return -1;
            }

            writerAndStreamIndex = createWriter(outputFilename.data(), format);
            if (writerAndStreamIndex.first.p == nullptr) {
                printf("ERROR: Unable to create video writer for \"%s\" with %ux%u resolution.  Exiting.\n", outputFilename.data(), format.width, format.height);
                fflush(stdout);
                return -1;
            }
        }

        uint64_t frameEndTime = (timeUnitsPerSecond * framei * format.fpsDenominator) / format.fpsNumerator;
        if (!writeFrame(writerAndStreamIndex.first.p, writerAndStreamIndex.second, imageData.data(), frameStartTime, frameEndTime, format)) {
            printf("ERROR: Failed to write frame %zu of \"%s\".  Exiting.\n", framei, outputFilename.data());
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

    if (writerAndStreamIndex.first.p != nullptr) {
        if (!hresultSuccess(writerAndStreamIndex.first->Finalize())) {
            printf("ERROR: Failed to finalize \"%s\".  Exiting.\n", outputFilename.data());
            fflush(stdout);
            return -1;
        }

        if (cancelled) {
            DeleteFile(outputFilename.data());
        }
    }

    return 0;
}
