#pragma once

#include <H5Cpp.h>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Bounded queue for async frame buffering
template <typename T> class BoundedQueue {
  public:
    explicit BoundedQueue(size_t maxSize) : maxSize_(maxSize), stopped_(false) {}

    ~BoundedQueue() { stop(); }

    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait if queue is full
        cvNotFull_.wait(lock, [this] { return queue_.size() < maxSize_ || stopped_; });

        if (stopped_) {
            return;
        }

        queue_.push(std::move(item));
        cvNotEmpty_.notify_one();
    }

    bool pop(T &item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait if queue is empty
        cvNotEmpty_.wait(lock, [this] { return !queue_.empty() || stopped_; });

        if (stopped_ && queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();
        cvNotFull_.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cvNotEmpty_.notify_all();
        cvNotFull_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

  private:
    std::queue<T> queue_;
    size_t maxSize_;
    mutable std::mutex mutex_;
    std::condition_variable cvNotEmpty_;
    std::condition_variable cvNotFull_;
    std::atomic<bool> stopped_;
};

// Frame data structure for HDF5 writing
struct H5FrameData {
    int frameNumber;
    int64_t timestampUs;         // Microseconds since epoch for precise synchronization
    std::vector<uint8_t> pixels; // BGRA pixel data
    int width;
    int height;
    std::map<std::string, std::string> memoryData;
    double frameCaptureMs;
    double memoryReadMs;
    double diskWriteMs;
    double totalLatencyMs;
};

// HDF5 recording writer with async queue
class H5RecordingWriter {
  public:
    H5RecordingWriter(const std::string &filepath, int width, int height,
                      const std::vector<std::string> &attributeNames, size_t queueSize = 120);
    ~H5RecordingWriter();

    // Queue a frame for writing (non-blocking)
    void QueueFrame(H5FrameData frameData);

    // Stop writing and wait for queue to drain
    void Finalize();

    // Get current queue size
    size_t GetQueueSize() const { return queue_.size(); }

    // Get total frames written
    int GetFramesWritten() const { return framesWritten_.load(); }

  private:
    void WriterThread();
    void WriteFrame(const H5FrameData &frameData);
    void InitializeDatasets();

    std::string filepath_;
    int width_;
    int height_;
    std::vector<std::string> attributeNames_;

    H5::H5File file_;
    H5::DataSet framesDataset_;
    H5::DataSet timestampsDataset_;
    H5::DataSet memoryDataset_;
    H5::DataSet latenciesDataset_;

    BoundedQueue<H5FrameData> queue_;
    std::thread writerThread_;
    std::atomic<int> framesWritten_;
    std::atomic<bool> finalized_;
};
