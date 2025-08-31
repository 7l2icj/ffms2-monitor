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

#include "differential_index.h"
#include <algorithm>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// Get the maximum file position from all tracks
int64_t GetMaxFilePosition(FFMS_Index *Index) {
    if (!Index) return 0;
    
    // Now we can access FilePos through the public API!
    int64_t maxPos = 0;
    int numTracks = FFMS_GetNumTracks(Index);
    
    for (int t = 0; t < numTracks; t++) {
        FFMS_Track *track = FFMS_GetTrackFromIndex(Index, t);
        if (!track) continue;
        
        int frameCount = FFMS_GetNumFrames(track);
        if (frameCount > 0) {
            // Get the last frame's info
            const FFMS_FrameInfo *lastFrame = FFMS_GetFrameInfo(track, frameCount - 1);
            if (lastFrame && lastFrame->FilePos > maxPos) {
                maxPos = lastFrame->FilePos;
            }
        }
    }
    
    return maxPos;
}

// Get resume information from existing index
IndexResumeInfo GetIndexResumeInfo(FFMS_Index *Index) {
    IndexResumeInfo info;
    
    if (!Index) {
        info.Valid = false;
        return info;
    }
    
    info.LastFilePosition = GetMaxFilePosition(Index);
    
    // Get frame count and last timing info
    int numTracks = FFMS_GetNumTracks(Index);
    for (int t = 0; t < numTracks; t++) {
        FFMS_Track *track = FFMS_GetTrackFromIndex(Index, t);
        if (!track) continue;
        
        int frameCount = FFMS_GetNumFrames(track);
        info.FrameCount = std::max(info.FrameCount, frameCount);
        
        if (frameCount > 0) {
            const FFMS_FrameInfo *lastFrame = FFMS_GetFrameInfo(track, frameCount - 1);
            if (lastFrame) {
                // Track the maximum PTS/DTS across all tracks
                if (lastFrame->PTS > info.LastPTS)
                    info.LastPTS = lastFrame->PTS;
                // Note: DTS is not directly available in FFMS_FrameInfo
                // We use PTS as approximation
                
                // For audio tracks, now we can get sample count through public API
                if (FFMS_GetTrackType(track) == FFMS_TYPE_AUDIO) {
                    info.LastSampleCount = std::max(info.LastSampleCount, 
                        lastFrame->SampleStart + (int64_t)lastFrame->SampleCount);
                }
            }
        }
    }
    
    info.Valid = (info.LastFilePosition > 0);
    return info;
}

// Custom indexing function that can resume from a specific position
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
) {
    // Use the new API with resume support!
    FFMS_Indexer *Indexer = nullptr;
    
    if (ResumeInfo.Valid && ExistingIndex && ResumeInfo.LastFilePosition > 0) {
        // Create indexer with resume position
        Indexer = FFMS_CreateIndexerWithProgress(SourceFile, 
            LAVFOpts.data(), LAVFOpts.size(), 
            ResumeInfo.LastFilePosition, 
            ExistingIndex,
            ErrorInfo);
            
        if (Indexer && IC) {
            std::cerr << "Differential indexing from byte position: " 
                      << ResumeInfo.LastFilePosition << std::endl;
        }
    } else {
        // Fall back to regular indexer
        Indexer = FFMS_CreateIndexer2(SourceFile, 
            LAVFOpts.data(), LAVFOpts.size(), ErrorInfo);
    }
    
    if (!Indexer) {
        return nullptr;
    }
    
    // Set progress callback
    FFMS_SetProgressCallback(Indexer, IC, ICPrivate);
    
    // Configure tracks to index
    if (IndexMask == -1) {
        FFMS_TrackTypeIndexSettings(Indexer, FFMS_TYPE_AUDIO, 1, 0);
    } else {
        for (int i = 0; i < static_cast<int>(sizeof(IndexMask) * 8); i++) {
            if ((IndexMask >> i) & 1)
                FFMS_TrackIndexSettings(Indexer, i, 1, 0);
        }
    }
    
    // Do the indexing (now with resume support if configured)
    FFMS_Index *NewIndex = FFMS_DoIndexing2(Indexer, IgnoreErrors, ErrorInfo);
    
    // Indexer is freed by DoIndexing2
    
    return NewIndex;
}

// Alternative approach: Manual differential indexing
// This would require direct access to FFmpeg APIs
FFMS_Index* ManualDifferentialIndexing(
    const char *SourceFile,
    FFMS_Index *ExistingIndex,
    const IndexResumeInfo &ResumeInfo,
    int IgnoreErrors,
    FFMS_ErrorInfo *ErrorInfo
) {
    // This implementation would:
    // 1. Open the file with av_open_input
    // 2. Seek to ResumeInfo.LastFilePosition
    // 3. Read packets starting from there
    // 4. Append frame info to a copy of ExistingIndex
    // 
    // This is complex and requires reimplementing much of FFMS_Indexer
    // Left as a TODO for future enhancement
    
    return nullptr;
}