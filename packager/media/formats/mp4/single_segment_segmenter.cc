// Copyright 2014 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/formats/mp4/single_segment_segmenter.h"

#include <algorithm>

#include "packager/media/base/buffer_writer.h"
#include "packager/media/base/media_stream.h"
#include "packager/media/base/muxer_options.h"
#include "packager/media/event/muxer_listener.h"
#include "packager/media/event/progress_listener.h"
#include "packager/media/file/file.h"
#include "packager/media/file/file_util.h"
#include "packager/media/formats/mp4/box_definitions.h"

namespace shaka {
namespace media {
namespace mp4 {

SingleSegmentSegmenter::SingleSegmentSegmenter(const MuxerOptions& options,
                                               std::unique_ptr<FileType> ftyp,
                                               std::unique_ptr<Movie> moov)
    : Segmenter(options, std::move(ftyp), std::move(moov)) {}

SingleSegmentSegmenter::~SingleSegmentSegmenter() {
  if (temp_file_)
    temp_file_.release()->Close();
  if (!temp_file_name_.empty()) {
    if (!File::Delete(temp_file_name_.c_str()))
      LOG(ERROR) << "Unable to delete temporary file " << temp_file_name_;
  }
}

bool SingleSegmentSegmenter::GetInitRange(size_t* offset, size_t* size) {
  // In Finalize, ftyp and moov gets written first so offset must be 0.
  *offset = 0;
  *size = ftyp()->ComputeSize() + moov()->ComputeSize();
  return true;
}

bool SingleSegmentSegmenter::GetIndexRange(size_t* offset, size_t* size) {
  // Index range is right after init range so the offset must be the size of
  // ftyp and moov.
  *offset = ftyp()->ComputeSize() + moov()->ComputeSize();
  *size = vod_sidx_->ComputeSize();
  return true;
}

Status SingleSegmentSegmenter::DoInitialize() {
  // Single segment segmentation involves two stages:
  //   Stage 1: Create media subsegments from media samples
  //   Stage 2: Update media header (moov) which involves copying of media
  //            subsegments
  // Assumes stage 2 takes similar amount of time as stage 1. The previous
  // progress_target was set for stage 1. Times two to account for stage 2.
  set_progress_target(progress_target() * 2);

  if (!TempFilePath(options().temp_dir, &temp_file_name_))
    return Status(error::FILE_FAILURE, "Unable to create temporary file.");
  temp_file_.reset(File::Open(temp_file_name_.c_str(), "w"));
  return temp_file_
             ? Status::OK
             : Status(error::FILE_FAILURE,
                      "Cannot open file to write " + temp_file_name_);
}

Status SingleSegmentSegmenter::DoFinalize() {
  DCHECK(temp_file_);
  DCHECK(ftyp());
  DCHECK(moov());
  DCHECK(vod_sidx_);

  // Close the temp file to prepare for reading later.
  if (!temp_file_.release()->Close()) {
    return Status(error::FILE_FAILURE,
                  "Cannot close the temp file " + temp_file_name_);
  }

  std::unique_ptr<File, FileCloser> file(
      File::Open(options().output_file_name.c_str(), "w"));
  if (file == NULL) {
    return Status(error::FILE_FAILURE,
                  "Cannot open file to write " + options().output_file_name);
  }

  LOG(INFO) << "Update media header (moov) and rewrite the file to '"
            << options().output_file_name << "'.";

  // Write ftyp, moov and sidx to output file.
  std::unique_ptr<BufferWriter> buffer(new BufferWriter());
  ftyp()->Write(buffer.get());
  moov()->Write(buffer.get());
  vod_sidx_->Write(buffer.get());
  Status status = buffer->WriteToFile(file.get());
  if (!status.ok())
    return status;

  // Load the temp file and write to output file.
  std::unique_ptr<File, FileCloser> temp_file(
      File::Open(temp_file_name_.c_str(), "r"));
  if (temp_file == NULL) {
    return Status(error::FILE_FAILURE,
                  "Cannot open file to read " + temp_file_name_);
  }

  // The target of 2nd stage of single segment segmentation.
  const uint64_t re_segment_progress_target = progress_target() * 0.5;

  const int kBufSize = 0x200000;  // 2MB.
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kBufSize]);
  while (true) {
    int64_t size = temp_file->Read(buf.get(), kBufSize);
    if (size == 0) {
      break;
    } else if (size < 0) {
      return Status(error::FILE_FAILURE,
                    "Failed to read file " + temp_file_name_);
    }
    int64_t size_written = file->Write(buf.get(), size);
    if (size_written != size) {
      return Status(error::FILE_FAILURE,
                    "Failed to write file " + options().output_file_name);
    }
    UpdateProgress(static_cast<double>(size) / temp_file->Size() *
                   re_segment_progress_target);
  }
  SetComplete();
  return Status::OK;
}

Status SingleSegmentSegmenter::DoFinalizeSegment() {
  DCHECK(sidx());
  DCHECK(fragment_buffer());
  // sidx() contains pre-generated segment references with one reference per
  // fragment. In VOD, this segment is converted into a subsegment, i.e. one
  // reference, which contains all the fragments in sidx().
  std::vector<SegmentReference>& refs = sidx()->references;
  SegmentReference& vod_ref = refs[0];
  uint64_t first_sap_time =
      refs[0].sap_delta_time + refs[0].earliest_presentation_time;
  for (uint32_t i = 1; i < refs.size(); ++i) {
    vod_ref.referenced_size += refs[i].referenced_size;
    // NOTE: We calculate subsegment duration based on the total duration of
    // this subsegment instead of subtracting earliest_presentation_time as
    // indicated in the spec.
    vod_ref.subsegment_duration += refs[i].subsegment_duration;
    vod_ref.earliest_presentation_time = std::min(
        vod_ref.earliest_presentation_time, refs[i].earliest_presentation_time);

    if (vod_ref.sap_type == SegmentReference::TypeUnknown &&
        refs[i].sap_type != SegmentReference::TypeUnknown) {
      vod_ref.sap_type = refs[i].sap_type;
      first_sap_time =
          refs[i].sap_delta_time + refs[i].earliest_presentation_time;
    }
  }
  // Calculate sap delta time w.r.t. earliest_presentation_time.
  if (vod_ref.sap_type != SegmentReference::TypeUnknown) {
    vod_ref.sap_delta_time =
        first_sap_time - vod_ref.earliest_presentation_time;
  }

  // Create segment if it does not exist yet.
  if (vod_sidx_ == NULL) {
    vod_sidx_.reset(new SegmentIndex());
    vod_sidx_->reference_id = sidx()->reference_id;
    vod_sidx_->timescale = sidx()->timescale;

    if (vod_ref.earliest_presentation_time > 0) {
      const double starting_time_in_seconds =
          static_cast<double>(vod_ref.earliest_presentation_time) /
          GetReferenceTimeScale();
      // Give a warning if it is significant.
      if (starting_time_in_seconds > 0.5) {
        // Note that DASH IF player requires presentationTimeOffset to be set in
        // Segment{Base,List,Template} if there is non-zero starting time. Since
        // current Chromium's MSE implementation uses DTS, the player expects
        // DTS to be used.
        LOG(WARNING) << "Warning! Non-zero starting time (in seconds): "
                     << starting_time_in_seconds
                     << ". Manual adjustment of presentationTimeOffset in "
                        "mpd might be necessary.";
      }
    }
    // Force earliest_presentation_time to start from 0 for VOD.
    vod_sidx_->earliest_presentation_time = 0;
  }
  vod_sidx_->references.push_back(vod_ref);

  // Append fragment buffer to temp file.
  size_t segment_size = fragment_buffer()->Size();
  Status status = fragment_buffer()->WriteToFile(temp_file_.get());
  if (!status.ok()) return status;

  UpdateProgress(vod_ref.subsegment_duration);
  if (muxer_listener()) {
    muxer_listener()->OnSampleDurationReady(sample_duration());
    muxer_listener()->OnNewSegment(options().output_file_name,
                                   vod_ref.earliest_presentation_time,
                                   vod_ref.subsegment_duration, segment_size);
  }
  return Status::OK;
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
