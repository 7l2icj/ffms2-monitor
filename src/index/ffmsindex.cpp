//  Copyright (c) 2008-2009 Karl Blomster
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "ffms.h"
#include "differential_index.h"

#ifdef _WIN32
#include "vsutf16.h"
#endif

#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <string>
#include <stdexcept>
#include <chrono>
#include <csignal>
#include <sys/stat.h>
#include <thread>

extern "C" {
#include <libavutil/dict.h>
}

namespace {

long long IndexMask = 0;
int Verbose = 0;
int IgnoreErrors = 0;
bool Overwrite = false;
bool PrintProgress = true;
bool WriteTC = false;
bool WriteKF = false;
int64_t ProgressInterval = 1000000; // One second
std::vector<FFMS_KeyValuePair> LAVFOpts;
std::string InputFile;
std::string CacheFile;
bool AppendMode = false;
bool MonitorMode = false;
int MonitorInterval = 10; // seconds
int IdleTimeout = 30; // seconds - exit if file doesn't change for this long
volatile bool KeepRunning = true;

struct Error {
    std::string msg;
    Error(const char *msg) : msg(msg) {}
    Error(const char *msg, FFMS_ErrorInfo const& e) : msg(msg) {
        this->msg.append(e.Buffer);
    }
};

struct Progress {
    int Percent;
    int64_t Time;
};

void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        KeepRunning = false;
    }
}

void PrintUsage() {
    std::cout <<
        "FFmpegSource2 indexing app\n"
        "Usage: ffmsindex [options] inputfile [outputfile]\n"
        "If no output filename is specified, inputfile.ffindex will be used.\n"
        "\n"
        "Options:\n"
        "-f        Force overwriting of existing index file, if any (default: no)\n"
        "-v        Set FFmpeg verbosity level. Can be repeated for more verbosity. (default: no messages printed)\n"
        "-p        Disable progress reporting. (default: progress reporting on)\n"
        "-c        Write timecodes for all video tracks to outputfile_track00.tc.txt (default: no)\n"
        "-k        Write keyframes for all video tracks to outputfile_track00.kf.txt (default: no)\n"
        "-t N      Set the audio indexing mask to N (-1 means index all tracks, 0 means index none, default: 0)\n"
        "-s N      Set audio decoding error handling. See the documentation for details. (default: 0)\n"
        "-u N      Set the progress update frequency in seconds. Set to 0 for every percent. (default: 1)\n"
        "-o string Set demuxer options to be used in the form of 'key=val:key=val'. (default: none)\n"
        "-a        Append mode: update existing index with new data (default: no)\n"
        "-m        Monitor mode: continuously update index for growing file (default: no)\n"
        "-i N      Monitor interval in seconds for monitor mode (default: 10)\n"
        "-w N      Idle timeout in seconds (exit if file unchanged, default: 30, 0=disabled)\n"
        "\n"
        "FFmpeg Demuxer Options:\n"
        "--enable_drefs\n"
        "--use_absolute_path\n"
        << std::endl;
}

int64_t getTimeInMicroSeconds() {
    static std::chrono::time_point<std::chrono::steady_clock> StartTime = std::chrono::steady_clock::now();
    std::chrono::time_point<std::chrono::steady_clock> Now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(Now - StartTime).count();
}

int64_t parseSecondsToMicroseconds(const char *str) {
    double val = std::strtod(str, nullptr);
    int64_t ret = (int64_t) (val * 1000000.0);
    return ret;
}

void freeDemuxerOpts() {
    for (FFMS_KeyValuePair pair : LAVFOpts) {
        free(const_cast<char *>(pair.Key));
        free(const_cast<char *>(pair.Value));
    }
    LAVFOpts.clear();
}

std::vector<FFMS_KeyValuePair> parseDemuxerOpts(const char *str) {
    AVDictionary *dict = nullptr;
    int ret = av_dict_parse_string(&dict, str, "=", ":", 0);
    if (ret < 0)
        throw Error("Cannot parse demuxer options.");

    AVDictionaryEntry *en = nullptr;
    while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)) != NULL) {
        FFMS_KeyValuePair pair;
        pair.Key = strdup(en->key);
        if (!pair.Key)
            goto fail;
        pair.Value = strdup(en->value);
        if (!pair.Value) {
            free(const_cast<char *>(pair.Key));
            goto fail;
        }
        LAVFOpts.push_back(pair);
    }
    return LAVFOpts;
fail:
    freeDemuxerOpts();
    throw Error("Could not allocate key/value pair.");
}

void ParseCMDLine(int argc, const char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        const char *Option = argv[i];
#define OPTION_ARG(dst, flag, parse) try { dst = parse(i + 1 < argc ? argv[i+1] : throw Error("Error: missing argument for -" flag)); i++; } catch (std::logic_error &) { throw Error("Error: invalid argument specified for -" flag); }

        if (!strcmp(Option, "-f")) {
            Overwrite = true;
        } else if (!strcmp(Option, "-v")) {
            Verbose++;
        } else if (!strcmp(Option, "-p")) {
            PrintProgress = false;
        } else if (!strcmp(Option, "-c")) {
            WriteTC = true;
        } else if (!strcmp(Option, "-k")) {
            WriteKF = true;
        } else if (!strcmp(Option, "-t")) {
            OPTION_ARG(IndexMask, "t", std::stoll);
        } else if (!strcmp(Option, "-s")) {
            OPTION_ARG(IgnoreErrors, "s", std::stoi);
        } else if (!strcmp(Option, "-u")) {
            OPTION_ARG(ProgressInterval, "u", parseSecondsToMicroseconds);
        } else if (!strcmp(Option, "-o")) {
            OPTION_ARG(LAVFOpts, "o", parseDemuxerOpts);
        } else if (!strcmp(Option, "-a")) {
            AppendMode = true;
        } else if (!strcmp(Option, "-m")) {
            MonitorMode = true;
        } else if (!strcmp(Option, "-i")) {
            OPTION_ARG(MonitorInterval, "i", std::stoi);
        } else if (!strcmp(Option, "-w")) {
            OPTION_ARG(IdleTimeout, "w", std::stoi);
        } else if (!strcmp(Option, "--enable_drefs")) {
            parseDemuxerOpts("enable_drefs=1");
        } else if (!strcmp(Option, "--use_absolute_path")) {
            parseDemuxerOpts("use_absolute_path=1");
        } else if (InputFile.empty()) {
            InputFile = Option;
        } else if (CacheFile.empty()) {
            CacheFile = Option;
        } else {
            std::cout << "Warning: ignoring unknown option " << Option << std::endl;
        }
    }

    if (IgnoreErrors < 0 || IgnoreErrors > 3)
        throw Error("Error: invalid error handling mode");
    if (InputFile.empty())
        throw Error("Error: no input file specified");
    if (MonitorInterval < 1)
        throw Error("Error: monitor interval must be at least 1 second");
    if (IdleTimeout < 0)
        throw Error("Error: idle timeout cannot be negative");

    if (CacheFile.empty()) {
        CacheFile = InputFile;
        CacheFile.append(".ffindex");
    }
}

int FFMS_CC UpdateProgress(int64_t Current, int64_t Total, void *Private) {
    if (!PrintProgress)
        return 0;

    int Percentage = int((double(Current) / double(Total)) * 100);

    if (Private) {
        Progress *LastProgress = (Progress *)Private;
        int64_t CurTime = getTimeInMicroSeconds();
        if (Percentage <= LastProgress->Percent || (LastProgress->Time != 0 && (CurTime - LastProgress->Time) <= ProgressInterval))
            return 0;
        LastProgress->Percent = Percentage;
        LastProgress->Time = CurTime;
    }

    std::cout << "Indexing, please wait... " << Percentage << "% \r" << std::flush;

    return 0;
}

std::string DumpFilename(FFMS_Track *Track, int TrackNum, const char *Suffix) {
    if (FFMS_GetTrackType(Track) != FFMS_TYPE_VIDEO || !FFMS_GetNumFrames(Track))
        return "";

    char tn[11];
    snprintf(tn, 11, "%02" PRIu32"", (uint32_t) TrackNum);
    return CacheFile + "_track" + tn + Suffix;
}

void DoIndexing() {
    char ErrorMsg[1024];
    FFMS_ErrorInfo E;
    E.Buffer = ErrorMsg;
    E.BufferSize = sizeof(ErrorMsg);

    Progress ProgressTracker = { 0, getTimeInMicroSeconds() };
    
    // For monitor mode, we need to track file size and idle time
    int64_t lastFileSize = -1;  // Initialize to -1 to detect first run
    int64_t lastIndexedPosition = 0;
    FFMS_Index *ExistingIndex = nullptr;
    int idleSeconds = 0;  // Track consecutive seconds without file growth
    
    // Check for existing index
    if (AppendMode || MonitorMode) {
        ExistingIndex = FFMS_ReadIndex(CacheFile.c_str(), &E);
        if (!ExistingIndex && AppendMode && !MonitorMode) {
            throw Error("Error: no existing index found for append mode");
        }
        // For monitor mode, it's OK if index doesn't exist yet
        
        // Get the last indexed position if we have an existing index
        if (ExistingIndex) {
            lastIndexedPosition = GetMaxFilePosition(ExistingIndex);
            if (PrintProgress && lastIndexedPosition > 0) {
                std::cout << "Resuming from byte position: " << lastIndexedPosition << std::endl;
            }
        }
    } else {
        // Normal mode - check if index exists
        FFMS_Index *TestIndex = FFMS_ReadIndex(CacheFile.c_str(), &E);
        if (TestIndex) {
            FFMS_DestroyIndex(TestIndex);
            if (!Overwrite)
                throw Error("Error: index file already exists, use -f if you are sure you want to overwrite it.");
        }
    }
    
    // Main indexing loop
    do {
        // Get current file size
        struct stat st;
        if (stat(InputFile.c_str(), &st) != 0) {
            if (ExistingIndex)
                FFMS_DestroyIndex(ExistingIndex);
            throw Error("Error: cannot stat input file");
        }
        int64_t currentFileSize = st.st_size;
        
        // Check if file has grown since last check
        if (MonitorMode) {
            // First run, always index
            if (lastFileSize == -1) {
                lastFileSize = currentFileSize;
                if (PrintProgress) {
                    std::cout << "Initial file size: " << currentFileSize << " bytes" << std::endl;
                }
            } else if (currentFileSize <= lastFileSize) {
                // File hasn't grown since last check
                idleSeconds += MonitorInterval;
                
                // Check for idle timeout
                if (IdleTimeout > 0 && idleSeconds >= IdleTimeout) {
                    if (PrintProgress) {
                        std::cout << "File unchanged for " << IdleTimeout 
                                 << " seconds. Exiting monitor mode." << std::endl;
                    }
                    break;
                }
                
                if (KeepRunning) {
                    if (PrintProgress) {
                        std::cout << "File size: " << currentFileSize 
                                 << " bytes (unchanged). Idle: " << idleSeconds << "/" 
                                 << (IdleTimeout > 0 ? std::to_string(IdleTimeout) : "∞")
                                 << " seconds" << std::endl;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(MonitorInterval));
                    continue;
                } else {
                    break; // Exit if signal received
                }
            }
            
            // File has grown, reset idle counter and proceed with indexing
            idleSeconds = 0;
            if (PrintProgress) {
                std::cout << "File grown from " << lastFileSize 
                         << " to " << currentFileSize << " bytes" << std::endl;
            }
        }
        
        lastFileSize = currentFileSize;
        
        UpdateProgress(0, 100, nullptr);
        
        FFMS_Index *NewIndex = nullptr;
        
        // Check if we can do differential indexing
        if (ExistingIndex && lastIndexedPosition > 0 && (AppendMode || MonitorMode)) {
            // Get resume information
            IndexResumeInfo resumeInfo = GetIndexResumeInfo(ExistingIndex);
            
            if (resumeInfo.Valid && PrintProgress) {
                std::cout << "Attempting differential indexing from position " 
                         << resumeInfo.LastFilePosition << std::endl;
            }
            
            // Try differential indexing
            NewIndex = DoDifferentialIndexing(
                InputFile.c_str(),
                ExistingIndex,
                resumeInfo,
                IgnoreErrors,
                IndexMask,
                LAVFOpts,
                UpdateProgress,
                &ProgressTracker,
                &E
            );
        }
        
        // Fall back to full indexing if differential failed or not applicable
        if (!NewIndex) {
            // Create indexer
            FFMS_Indexer *Indexer = FFMS_CreateIndexer2(InputFile.c_str(), LAVFOpts.data(), LAVFOpts.size(), &E);
            if (Indexer == nullptr) {
                if (ExistingIndex)
                    FFMS_DestroyIndex(ExistingIndex);
                throw Error("\nFailed to initialize indexing: ", E);
            }
            
            FFMS_SetProgressCallback(Indexer, UpdateProgress, &ProgressTracker);
            
            // Configure tracks to index
            if (IndexMask == -1)
                FFMS_TrackTypeIndexSettings(Indexer, FFMS_TYPE_AUDIO, 1, 0);
            
            for (int i = 0; i < static_cast<int>(sizeof(IndexMask) * 8); i++) {
                if ((IndexMask >> i) & 1)
                    FFMS_TrackIndexSettings(Indexer, i, 1, 0);
            }
            
            // Do the indexing
            NewIndex = FFMS_DoIndexing2(Indexer, IgnoreErrors, &E);
            
            // The indexer is always freed
            Indexer = nullptr;
        }
        
        if (NewIndex == nullptr) {
            if (ExistingIndex)
                FFMS_DestroyIndex(ExistingIndex);
            freeDemuxerOpts();
            throw Error("\nIndexing error: ", E);
        }
        
        UpdateProgress(100, 100, nullptr);
        std::cout << std::endl;
        
        // Update the last indexed position
        lastIndexedPosition = GetMaxFilePosition(NewIndex);
        
        // Replace existing index with new one
        if (ExistingIndex) {
            FFMS_DestroyIndex(ExistingIndex);
        }
        ExistingIndex = NewIndex;
        
        // Write timecodes if requested
        if (WriteTC) {
            if (PrintProgress)
                std::cout << "Writing timecodes... ";
            int NumTracks = FFMS_GetNumTracks(ExistingIndex);
            for (int t = 0; t < NumTracks; t++) {
                FFMS_Track *Track = FFMS_GetTrackFromIndex(ExistingIndex, t);
                std::string Filename = DumpFilename(Track, t, ".tc.txt");
                if (!Filename.empty()) {
                    if (FFMS_WriteTimecodes(Track, Filename.c_str(), &E))
                        std::cout << std::endl << "Failed to write timecodes file "
                        << Filename << ": " << E.Buffer << std::endl;
                }
            }
            if (PrintProgress)
                std::cout << "done." << std::endl;
        }
        
        // Write keyframes if requested
        if (WriteKF) {
            if (PrintProgress)
                std::cout << "Writing keyframes... ";
            int NumTracks = FFMS_GetNumTracks(ExistingIndex);
            for (int t = 0; t < NumTracks; t++) {
                FFMS_Track *Track = FFMS_GetTrackFromIndex(ExistingIndex, t);
                std::string Filename = DumpFilename(Track, t, ".kf.txt");
                if (!Filename.empty()) {
                    std::ofstream kf(Filename.c_str());
                    kf << "# keyframe format v1\n"
                        "fps 0\n";
                    
                    int FrameCount = FFMS_GetNumFrames(Track);
                    for (int CurFrameNum = 0; CurFrameNum < FrameCount; CurFrameNum++) {
                        if (FFMS_GetFrameInfo(Track, CurFrameNum)->KeyFrame)
                            kf << CurFrameNum << "\n";
                    }
                }
            }
            if (PrintProgress)
                std::cout << "done.    " << std::endl;
        }
        
        // Write the index
        if (PrintProgress)
            std::cout << "Writing index... ";
        
        int error = FFMS_WriteIndex(CacheFile.c_str(), ExistingIndex, &E);
        if (error) {
            FFMS_DestroyIndex(ExistingIndex);
            freeDemuxerOpts();
            throw Error("Error writing index: ", E);
        }
        
        if (PrintProgress) {
            std::cout << "done. (indexed up to byte " << lastIndexedPosition << ")" << std::endl;
        }
        
        // In monitor mode, sleep before next iteration
        if (MonitorMode && KeepRunning) {
            if (PrintProgress)
                std::cout << "Monitoring mode: waiting " << MonitorInterval << " seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(MonitorInterval));
        }
        
    } while (MonitorMode && KeepRunning);
    
    // Clean up
    if (ExistingIndex)
        FFMS_DestroyIndex(ExistingIndex);
    
    freeDemuxerOpts();
    
    if (PrintProgress && MonitorMode)
        std::cout << "Monitoring stopped." << std::endl;
}

} // namespace {

#ifdef _WIN32
int wmain(int argc, const wchar_t *_argv[]) {
    std::vector<const char *> StringPtrs(argc);
    std::vector<std::string> StringStorage(argc);

    for (int i = 0; i < argc; i++) {
        StringStorage[i] = utf16_to_utf8(_argv[i]);
        StringPtrs[i] = StringStorage[i].c_str();
    }

    const char **argv = StringPtrs.data();
#else
int main(int argc, const char *argv[]) {
#endif
    try {
        if (argc <= 1) {
            PrintUsage();
            return 0;
        }

        ParseCMDLine(argc, argv);
    } catch (Error const& e) {
        std::cout << e.msg << std::endl << std::flush;
        return 1;
    }

    // Set up signal handlers for graceful shutdown
    if (MonitorMode) {
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);
    }

    FFMS_Init(0, 0);

    switch (Verbose) {
    case 0: FFMS_SetLogLevel(FFMS_LOG_QUIET); break;
    case 1: FFMS_SetLogLevel(FFMS_LOG_WARNING); break;
    case 2: FFMS_SetLogLevel(FFMS_LOG_INFO); break;
    case 3:	FFMS_SetLogLevel(FFMS_LOG_VERBOSE); break;
    default: FFMS_SetLogLevel(FFMS_LOG_DEBUG); // if user used -v 4 or more times, he deserves the spam
    }

    try {
        DoIndexing();
    } catch (Error const& e) {
        std::cout << e.msg << std::endl << std::flush;
        return 1;
    }

    return 0;
}
