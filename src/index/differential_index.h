//  Copyright (c) 2024
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

#ifndef DIFFERENTIAL_INDEX_H
#define DIFFERENTIAL_INDEX_H

#include "ffms.h"
#include <cstdint>
#include <vector>

// FFMS_CC might not be defined, so define it if needed
#ifndef FFMS_CC
#ifdef _WIN32
#define FFMS_CC __stdcall
#else
#define FFMS_CC
#endif
#endif

// Structure to hold resume state from existing index
struct IndexResumeInfo {
    int64_t LastFilePosition;    // Last indexed byte position in file
    int64_t LastPTS;              // Last PTS for each track
    int64_t LastDTS;              // Last DTS for each track
    int64_t LastSampleCount;      // Last audio sample count for audio tracks
    int FrameCount;               // Number of frames already indexed
    bool Valid;                   // Whether the info is valid
    
    IndexResumeInfo() : LastFilePosition(0), LastPTS(0), LastDTS(0), 
                       LastSampleCount(0), FrameCount(0), Valid(false) {}
};

// Get resume information from an existing index
IndexResumeInfo GetIndexResumeInfo(FFMS_Index *Index);

// Type definition for index callback - matching the signature from ffms.h
typedef int (FFMS_CC *TIndexCallback)(int64_t Current, int64_t Total, void *ICPrivate);

// Perform differential indexing
FFMS_Index* DoDifferentialIndexing(
    const char *SourceFile,
    FFMS_Index *ExistingIndex,
    const IndexResumeInfo &ResumeInfo,
    int IgnoreErrors,
    int IndexMask,
    const std::vector<FFMS_KeyValuePair> &LAVFOpts,
    TIndexCallback IC,
    void *ICPrivate,
    FFMS_ErrorInfo *ErrorInfo
);

// Get the maximum file position from all tracks in an index
int64_t GetMaxFilePosition(FFMS_Index *Index);

#endif // DIFFERENTIAL_INDEX_H