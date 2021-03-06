#include <QtDebug>
#include <QFileInfo>
#include <QMutexLocker>

#include "control/controlobject.h"

#include "engine/cachingreaderworker.h"
#include "sources/soundsourceproxy.h"
#include "util/compatibility.h"
#include "util/event.h"
#include "util/logger.h"


namespace {

mixxx::Logger kLogger("CachingReaderWorker");

} // anonymous namespace

CachingReaderWorker::CachingReaderWorker(
        QString group,
        FIFO<CachingReaderChunkReadRequest>* pChunkReadRequestFIFO,
        FIFO<ReaderStatusUpdate>* pReaderStatusFIFO)
        : m_group(group),
          m_tag(QString("CachingReaderWorker %1").arg(m_group)),
          m_pChunkReadRequestFIFO(pChunkReadRequestFIFO),
          m_pReaderStatusFIFO(pReaderStatusFIFO),
          m_newTrackAvailable(false),
          m_stop(0) {
}

CachingReaderWorker::~CachingReaderWorker() {
}

ReaderStatusUpdate CachingReaderWorker::processReadRequest(
        const CachingReaderChunkReadRequest& request) {
    CachingReaderChunk* pChunk = request.chunk;
    DEBUG_ASSERT(pChunk);

    // Before trying to read any data we need to check if the audio source
    // is available and if any audio data that is needed by the chunk is
    // actually available.
    const auto chunkFrameIndexRange = pChunk->frameIndexRange(m_pAudioSource);
    if (intersect(chunkFrameIndexRange, m_readableFrameIndexRange).empty()) {
        return ReaderStatusUpdate(CHUNK_READ_INVALID, pChunk, m_readableFrameIndexRange);
    }

    // Try to read the data required for the chunk from the audio source
    // and adjust the max. readable frame index if decoding errors occur.
    const mixxx::IndexRange bufferedFrameIndexRange = pChunk->bufferSampleFrames(
            m_pAudioSource,
            mixxx::SampleBuffer::WritableSlice(m_tempReadBuffer));
    ReaderStatus status = bufferedFrameIndexRange.empty() ? CHUNK_READ_EOF : CHUNK_READ_SUCCESS;
    if (chunkFrameIndexRange != bufferedFrameIndexRange) {
        kLogger.warning()
                << "Failed to read chunk samples for frame index range:"
                << "actual =" << bufferedFrameIndexRange
                << ", expected =" << chunkFrameIndexRange;
        if (bufferedFrameIndexRange.empty()) {
            // Adjust upper bound: Consider all audio data following
            // the read position until the end as unreadable
            m_readableFrameIndexRange.shrinkBack(m_readableFrameIndexRange.end() - chunkFrameIndexRange.start());
            status = CHUNK_READ_INVALID; // not EOF (see above)
        } else {
            // Adjust lower bound of readable audio data
            if (chunkFrameIndexRange.start() < bufferedFrameIndexRange.start()) {
                m_readableFrameIndexRange.shrinkFront(bufferedFrameIndexRange.start() - m_readableFrameIndexRange.start());
            }
            // Adjust upper bound of readable audio data
            if (chunkFrameIndexRange.end() > bufferedFrameIndexRange.end()) {
                m_readableFrameIndexRange.shrinkBack(m_readableFrameIndexRange.end() - bufferedFrameIndexRange.end());
            }
        }
        kLogger.warning()
                << "Readable frames in audio source reduced to"
                << m_readableFrameIndexRange
                << "from originally"
                << m_pAudioSource->frameIndexRange();
    }
    return ReaderStatusUpdate(status, pChunk, m_readableFrameIndexRange);
}

// WARNING: Always called from a different thread (GUI)
void CachingReaderWorker::newTrack(TrackPointer pTrack) {
    QMutexLocker locker(&m_newTrackMutex);
    m_pNewTrack = pTrack;
    m_newTrackAvailable = true;
}

void CachingReaderWorker::run() {
    unsigned static id = 0; //the id of this thread, for debugging purposes
    QThread::currentThread()->setObjectName(QString("CachingReaderWorker %1").arg(++id));

    CachingReaderChunkReadRequest request;

    Event::start(m_tag);
    while (!load_atomic(m_stop)) {
        if (m_newTrackAvailable) {
            TrackPointer pLoadTrack;
            { // locking scope
                QMutexLocker locker(&m_newTrackMutex);
                pLoadTrack = m_pNewTrack;
                m_pNewTrack.reset();
                m_newTrackAvailable = false;
            } // implicitly unlocks the mutex
            loadTrack(pLoadTrack);
        } else if (m_pChunkReadRequestFIFO->read(&request, 1) == 1) {
            // Read the requested chunk and send the result
            const ReaderStatusUpdate update(processReadRequest(request));
            m_pReaderStatusFIFO->writeBlocking(&update, 1);
        } else {
            Event::end(m_tag);
            m_semaRun.acquire();
            Event::start(m_tag);
        }
    }
}

namespace {

mixxx::AudioSourcePointer openAudioSourceForReading(const TrackPointer& pTrack, const mixxx::AudioSource::OpenParams& params) {
    auto pAudioSource = SoundSourceProxy(pTrack).openAudioSource(params);
    if (!pAudioSource) {
        kLogger.warning() << "Failed to open file:" << pTrack->getLocation();
    }
    return pAudioSource;
}

} // anonymous namespace

void CachingReaderWorker::loadTrack(const TrackPointer& pTrack) {
    ReaderStatusUpdate status;
    status.status = TRACK_NOT_LOADED;

    if (!pTrack) {
        // Unload track
        m_pAudioSource.reset(); // Close open file handles
        m_readableFrameIndexRange = mixxx::IndexRange();
        m_pReaderStatusFIFO->writeBlocking(&status, 1);
        return;
    }

    // Emit that a new track is loading, stops the current track
    emit(trackLoading());

    QString filename = pTrack->getLocation();
    if (filename.isEmpty() || !pTrack->exists()) {
        // Must unlock before emitting to avoid deadlock
        kLogger.debug() << m_group << "loadTrack() load failed for\""
                 << filename << "\", unlocked reader lock";
        m_pReaderStatusFIFO->writeBlocking(&status, 1);
        emit(trackLoadFailed(
            pTrack, QString("The file '%1' could not be found.")
                    .arg(QDir::toNativeSeparators(filename))));
        return;
    }

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(CachingReaderChunk::kChannels);
    m_pAudioSource = openAudioSourceForReading(pTrack, config);
    if (!m_pAudioSource) {
        m_readableFrameIndexRange = mixxx::IndexRange();
        // Must unlock before emitting to avoid deadlock
        kLogger.debug() << m_group << "loadTrack() load failed for\""
                 << filename << "\", file invalid, unlocked reader lock";
        m_pReaderStatusFIFO->writeBlocking(&status, 1);
        emit(trackLoadFailed(
            pTrack, QString("The file '%1' could not be loaded.").arg(filename)));
        return;
    }

    const SINT tempReadBufferSize = m_pAudioSource->frames2samples(CachingReaderChunk::kFrames);
    if (m_tempReadBuffer.size() != tempReadBufferSize) {
        mixxx::SampleBuffer(tempReadBufferSize).swap(m_tempReadBuffer);
    }

    // Initially assume that the complete content offered by audio source
    // is available for reading. Later if read errors occur this value will
    // be decreased to avoid repeated reading of corrupt audio data.
    m_readableFrameIndexRange = m_pAudioSource->frameIndexRange();

    status.readableFrameIndexRange = m_readableFrameIndexRange;
    status.status = TRACK_LOADED;
    m_pReaderStatusFIFO->writeBlocking(&status, 1);

    // Clear the chunks to read list.
    CachingReaderChunkReadRequest request;
    while (m_pChunkReadRequestFIFO->read(&request, 1) == 1) {
        kLogger.debug() << "Cancelling read request for " << request.chunk->getIndex();
        status.status = CHUNK_READ_INVALID;
        status.chunk = request.chunk;
        m_pReaderStatusFIFO->writeBlocking(&status, 1);
    }

    // Emit that the track is loaded.
    const SINT sampleCount =
            CachingReaderChunk::frames2samples(
                    m_pAudioSource->frameLength());
    emit(trackLoaded(pTrack, m_pAudioSource->sampleRate(), sampleCount));
}

void CachingReaderWorker::quitWait() {
    m_stop = 1;
    m_semaRun.release();
    wait();
}
