//#define LOG_NDEBUG 0
#define LOG_TAG "silk-capture-segmenter"
#include <log/log.h>

#include "MPEG4SegmenterDASH.h"

#include <cutils/properties.h>
#include <include/avc_utils.h>
#include <media/mediarecorder.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#ifdef TARGET_GE_NOUGAT
#include <media/stagefright/SimpleDecodingSource.h>
#else
#include <media/stagefright/OMXCodec.h>
#endif

#include "MPEG4SegmentDASHWriter.h"
#include "CaptureDataSocket.h"

// Normally "exported" from AACEncoder.h, but we can't include that here.
enum { kNumSamplesPerFrame = 1024 };

using namespace android;

//--------------------------------------------------
//
class MediaSourceWrapper : public MediaSource {
public:
  MediaSourceWrapper(const sp<MediaSource>& source)
    : mSource(source)
  { }

  virtual ~MediaSourceWrapper() {
  }

  virtual status_t start(MetaData *params = NULL) {
    return mSource->start(params);
  }

  virtual status_t stop() {
    return mSource->stop();
  }

  virtual sp<MetaData> getFormat() {
    return mSource->getFormat();
  }

  virtual status_t read(MediaBuffer **buffer, const ReadOptions *options) {
    return mSource->read(buffer, options);
  }

protected:
  sp<MediaSource> mSource;

  DISALLOW_EVIL_CONSTRUCTORS(MediaSourceWrapper);
};

//--------------------------------------------------
//
// TODO: rename this when SlicedMP4Encoder goes away
class PutBackWrapper2 : public MediaSourceWrapper {
public:
  PutBackWrapper2(const sp<MediaSource>& source)
    : MediaSourceWrapper(source)
  { }

  virtual ~PutBackWrapper2();
  virtual status_t read(MediaBuffer **buffer, const ReadOptions *options);
  void putBack(MediaBuffer **buffer);

private:
  Vector<MediaBuffer*> mStash;

  DISALLOW_EVIL_CONSTRUCTORS(PutBackWrapper2);
};

PutBackWrapper2::~PutBackWrapper2() {
  for (size_t n = 0; n < mStash.size(); ++n) {
    mStash[n]->release();
  }
}

status_t PutBackWrapper2::read(MediaBuffer **buffer,
                              const ReadOptions *options) {
  if (mStash.size() > 0) {
    *buffer = mStash.top();
    mStash.pop();
    return OK;
  }
  return mSource->read(buffer, options);
}

void PutBackWrapper2::putBack(MediaBuffer **buffer) {
  mStash.push(*buffer);
  *buffer = NULL;
}

//--------------------------------------------------
//
class EncoderProgress {
public:
  enum ProgressType {
    PROGRESS_NONE,
    PROGRESS_SYNC_FRAME, PROGRESS_DELTA_FRAME, PROGRESS_END_OF_STREAM
  };

  virtual ~EncoderProgress() { };

  void addListener(EncoderProgress *listener) {
    mListeners.push(listener);
  }

  virtual void handleProgressEvent(int64_t timeUs, ProgressType type) {
    (void) timeUs;
    (void) type;
    ALOGW("Encoder progress notified to non-listener?");
  }

  void notifyListeners(int64_t timeUs, ProgressType type);

private:
  Vector<EncoderProgress*> mListeners;
};

void EncoderProgress::notifyListeners(int64_t timeUs, ProgressType type) {
  ALOGV("  notifying listeners of %d at time %lld us", type, timeUs);
  for (size_t n = 0; n < mListeners.size(); ++n) {
    mListeners[n]->handleProgressEvent(timeUs, type);
  }
}

//--------------------------------------------------
//
class VideoSegmenter : public MediaSourceWrapper,
                       public EncoderProgress {
public:
  VideoSegmenter(
    const sp<PutBackWrapper2>& source,
    const sp<MediaCodec>& videoMediaCodec,
    int framesPerVideoSegment
  )
    : MediaSourceWrapper(source)
    , mVideoMediaCodec(videoMediaCodec)
    , mFramesPerVideoSegment(framesPerVideoSegment)
    , mFrameCount()
  { }

  virtual status_t stop() {
    // Don't stop the underlying stream.
    return OK;
  }

  virtual status_t read(MediaBuffer **buffer, const ReadOptions *options);

private:
  PutBackWrapper2* putbackWrapper() {
    return static_cast<PutBackWrapper2*>(mSource.get());
  }

  const sp<MediaCodec> mVideoMediaCodec;
  int mFramesPerVideoSegment;
  int mFrameCount;
};

status_t VideoSegmenter::read(
  MediaBuffer **buffer,
  const ReadOptions *options
)  {
  status_t err = mSource->read(buffer, options);
  if (err != OK) {
    ALOGE("Unexpected error from h264 encoder: %d", err);
    return err;
  }

  int64_t timeUs;
  if (!(*buffer)->meta_data()->findInt64(kKeyTime, &timeUs)) {
    timeUs = 0;
  }

  int32_t isCodecConfig;
  if ((*buffer)->meta_data()->findInt32(kKeyIsCodecConfig, &isCodecConfig)
      && isCodecConfig) {
    // The codec config is only sent at the beginning of the codec
    // stream, but we're segmenting this stream for multiple
    // containers, each of which needs to see the codec config.  So we
    // stash the codec config in the source format where the container
    // code can find it.
    sp<ABuffer> au = new ABuffer(
      static_cast<char *>((*buffer)->data()) + (*buffer)->range_offset(),
      (*buffer)->range_length());
    sp<MetaData> meta = MakeAVCCodecSpecificData(au);
    const void *data;
    size_t size;
    uint32_t type;
    if (meta->findData(kKeyAVCC, &type, &data, &size)) {
      mSource->getFormat()->setData(kKeyAVCC, type, data, size);
    } else {
      ALOGE("Unable to find AVCC in AVC codec data");
      // TODO: is there any way to recover from this?
    }
  }

  mFrameCount++;
  int32_t isSyncFrame = false;
  (*buffer)->meta_data()->findInt32(kKeyIsSyncFrame, &isSyncFrame);

  ALOGV("Frame %d of %d", mFrameCount, mFramesPerVideoSegment);
  if (isSyncFrame) {
    notifyListeners(timeUs, PROGRESS_SYNC_FRAME);
    if (mFrameCount >= mFramesPerVideoSegment) {
      ALOGD("Ending video segment with %d frames", mFrameCount);
      putbackWrapper()->putBack(buffer);
      notifyListeners(timeUs, PROGRESS_END_OF_STREAM);
      return ERROR_END_OF_STREAM;
    }
  } else {
    if (mFrameCount >= mFramesPerVideoSegment) {
      ALOGD("Time to end video segment, requesting sync frame");
      mVideoMediaCodec->requestIDRFrame();
    }
  }
  notifyListeners(timeUs, PROGRESS_DELTA_FRAME);
  return OK;
}

//--------------------------------------------------
//
class AudioSegmenter : public MediaSourceWrapper,
                       public EncoderProgress {
public:
  AudioSegmenter(const sp<PutBackWrapper2>& source,
                 EncoderProgress* progressEmitter)
    : MediaSourceWrapper(source)
    , mVideoProgressTimeUs()
    , mVideoProgressType(PROGRESS_NONE)
    , mAudioReadTimeUs()
    , mSampleRate()
    , mStashedCodecConfig()
  {
    CHECK(source->getFormat()->findInt32(kKeySampleRate, &mSampleRate));
    progressEmitter->addListener(this);
  }

  virtual status_t stop() {
    // Don't stop the underlying stream.
    return OK;
  }

  virtual status_t read(MediaBuffer **buffer, const ReadOptions *options);
  virtual void handleProgressEvent(int64_t timeUs, ProgressType type);

private:
  PutBackWrapper2* putbackWrapper() {
    return static_cast<PutBackWrapper2*>(mSource.get());
  }

  Mutex mLock;
  Condition mWaitForProgress;
  // The video progress time is an estimation of the end of the video
  // track.  The invariant of the audio segmenter is that the audio
  // track it produces starts at or after the beginning of the video
  // track, and ends at or after the end of the video track.  When the
  // video encoder produces a frame, we know that the video track must
  // last at least as long as up to that frame's timestamp.  So that's
  // a signal for the audio segmenter to read samples up until past
  // that timestamp.  So generally the "video progress time" is an
  // *under*-estimate of the end of the video track.
  //
  // The exception is for the very last progress notification,
  // `PROGRESS_END_OF_STREAM` below.  In that case, the timestamp is
  // for the exact end of the video segment.  So after the audio
  // segmenter reads samples up to or past that timestamp, we've fixed
  // up the segmenter invariant the last time and are finished.
  //
  // Note that the "audio read time" is always the exact end of the
  // audio track that's been read so far.  Unlike the video progress
  // time, which is a best current guess of where the end of the video
  // segment will be (until `PROGRESS_END_OF_STREAM`).
  int64_t mVideoProgressTimeUs;
  ProgressType mVideoProgressType;
  int64_t mAudioReadTimeUs;
  int32_t mSampleRate;
  MediaBuffer* mStashedCodecConfig;

  DISALLOW_EVIL_CONSTRUCTORS(AudioSegmenter);
};

status_t AudioSegmenter::read(MediaBuffer **buffer,
                              const ReadOptions *options) {
  // Wait until video encoder progress gets ahead of us.
  ALOGV("AAC: waiting for progress at time %lld", mAudioReadTimeUs);
  int64_t progressTimeUs;
  ProgressType progressType;
  {
    Mutex::Autolock lock(mLock);

    progressTimeUs = mVideoProgressTimeUs;
    progressType = mVideoProgressType;
    while (progressTimeUs < mAudioReadTimeUs
           && progressType != PROGRESS_END_OF_STREAM) {
      mWaitForProgress.wait(mLock);

      progressTimeUs = mVideoProgressTimeUs;
      progressType = mVideoProgressType;
    }
  }

  if (PROGRESS_END_OF_STREAM == progressType
      && progressTimeUs < mAudioReadTimeUs) {
    ALOGV("AAC: done!  progress to %lld, read to %lld",
          progressTimeUs, mAudioReadTimeUs);
    CHECK(mStashedCodecConfig);
    // Put the codec config back in the stream so that it's the first
    // buffer read() for the next segment.
    putbackWrapper()->putBack(&mStashedCodecConfig);
    return ERROR_END_OF_STREAM;
  }

  // Read the next buffer.  This is 1024 samples with AAC, so at 8KHz
  // sample rate will cover 128ms.
  ALOGV("AAC: reading new buffer to catch up to progress %lld", progressTimeUs);
  status_t err = mSource->read(buffer, options);
  if (err != OK) {
    ALOGE("Unexpected error from AAC encoder: %d", err);
    return err;
  }

  int32_t isCodecConfig;
  if ((*buffer)->meta_data()->findInt32(kKeyIsCodecConfig, &isCodecConfig)
      && isCodecConfig) {
    // See note above on VideoSegmenter about the codec config.
    // Unfortunately, the segmenter code that tries to find the codec
    // config in the *AAC* metadata doesn't work; it infers the wrong
    // AAC encoder profile.  So we apply a different hack here that
    // copies aside the code config buffer, and then puts it back in
    // the stream *just* before we start the next segment.
    MediaBuffer* buf = *buffer;
    mStashedCodecConfig = new MediaBuffer(buf->range_length());
    memcpy(mStashedCodecConfig->data(),
           (const uint8_t*)buf->data() + buf->range_offset(),
           mStashedCodecConfig->size());
    mStashedCodecConfig->meta_data()->setInt32(kKeyIsCodecConfig, true);
    ALOGV("Created up static AAC codec config");
    return OK;
  }

  int64_t startTimeUs;
  // Can we ever see an encoded sample buffer without a timestamp?
  CHECK((*buffer)->meta_data()->findInt64(kKeyTime, &startTimeUs));

  int64_t driftTimeUs;
  if ((*buffer)->meta_data()->findInt64(kKeyDriftTime, &driftTimeUs) &&
      driftTimeUs) {
    ALOGV("Adjusting time (%lld) by drift (%lld)", startTimeUs, driftTimeUs);
    startTimeUs += driftTimeUs;
  }

  ALOGV("  AAC progress: startTimeUs is %lld", startTimeUs);
  int64_t frameDurationUs = kNumSamplesPerFrame * 1e6 / mSampleRate;
  mAudioReadTimeUs = startTimeUs + frameDurationUs;

  return OK;
}

void AudioSegmenter::handleProgressEvent(int64_t timeUs, ProgressType type) {
  ALOGV("AAC: video encoder notifies %d at time %lld us", type, timeUs);
  
  Mutex::Autolock lock(mLock);
  mVideoProgressTimeUs = timeUs;
  mVideoProgressType = type;
  mWaitForProgress.signal();
}

//--------------------------------------------------
//
static void writerDecStrong(void* data) {
  MPEG4SegmentDASHWriter *writer = static_cast<MPEG4SegmentDASHWriter*>(data);
  writer->decStrong(nullptr);
};

MPEG4SegmenterDASH::MPEG4SegmenterDASH(
  const sp<MediaSource>& videoMediaSource,
  const sp<MediaCodec>& videoMediaCodec,
  int framesPerVideoSegment,
  const sp<MediaSource>& audioMediaSource,
  capture::datasocket::Channel* channel,
  bool initalMute
)
  : mVideoSource(new PutBackWrapper2(videoMediaSource))
  , mAudioSource(new PutBackWrapper2(audioMediaSource))
  , mChannel(channel)
  , mAudioMute(initalMute)
  , mVideoMediaCodec(videoMediaCodec)
  , mFramesPerVideoSegment(framesPerVideoSegment)
{}

bool MPEG4SegmenterDASH::threadLoop() {
  for (;;) {
    sp<VideoSegmenter> videoSource(
      new VideoSegmenter(
        mVideoSource,
        mVideoMediaCodec,
        mFramesPerVideoSegment
      )
    );
    sp<MediaSource> audioSource(
      new AudioSegmenter(mAudioSource, videoSource.get())
    );
    sp<MPEG4SegmentDASHWriter> writer = new MPEG4SegmentDASHWriter();
    writer->init(videoSource, &audioSource, mAudioMute);

    sp<MetaData> params = new MetaData();
    params->setInt32(kKeyFileType, OUTPUT_FORMAT_MPEG_4);

    timeval when;
    gettimeofday(&when, NULL);

    CHECK_EQ(writer->start(params.get()), OK);
    writer->waitForEOS();

    status_t err = writer->stop();
    if (err == OK) {
      // The "key track" is the video track.  The key track starts at
      // time 0 in the segment.  The duration isn't particularly
      // meaningful for DASH playback because segments overlap during
      // playback, but it's useful for approximate search of video
      // segments in the metadata DB.
      int64_t videoDurationUs = writer->getKeyTrackDurationUs();
      // (We won't overflow 31 bits unless the video duration is
      // > 35,000 hours ~= 4 years.)
      int32_t videoDurationMs = int32_t(videoDurationUs / 1000LL);
      // Write size and .mp4 data
      writer->incStrong(this);

      mChannel->send(
        capture::datasocket::TAG_MP4,
        when,
        videoDurationMs,
        writer->data().array(),
        writer->data().size(),
        writerDecStrong,
        writer.get()
      );
    } else {
      ALOGW("MPEG4SegmenterDASH stop failed with %d. No video data sent", err);
    }
  }
}
