#include "h5_recording_writer.h"
#include <spdlog/spdlog.h>
#include <algorithm>

H5RecordingWriter::H5RecordingWriter(const std::string &filepath, int width, int height,
                                     const std::vector<std::string> &attributeNames,
                                     size_t queueSize)
    : filepath_(filepath), width_(width), height_(height), 
      attributeNames_(attributeNames), queue_(queueSize), 
      framesWritten_(0), finalized_(false) {
    
    try {
        // Create HDF5 file
        file_ = H5::H5File(filepath_, H5F_ACC_TRUNC);
        
        // Initialize datasets
        InitializeDatasets();
        
        // Start writer thread
        writerThread_ = std::thread(&H5RecordingWriter::WriterThread, this);
        
        spdlog::info("H5RecordingWriter initialized: {}", filepath_);
        spdlog::info("Frame dimensions: {}x{}", width_, height_);
        spdlog::info("Queue size: {}", queueSize);
        
    } catch (const H5::Exception &e) {
        spdlog::error("Failed to create HDF5 file: {}", e.getCDetailMsg());
        throw;
    }
}

H5RecordingWriter::~H5RecordingWriter() {
    if (!finalized_) {
        Finalize();
    }
}

void H5RecordingWriter::InitializeDatasets() {
    try {
        // 1. Frames dataset: [N, height, width, 4] uint8 with chunking and compression
        hsize_t frameDims[4] = {0, static_cast<hsize_t>(height_), 
                                static_cast<hsize_t>(width_), 4};
        hsize_t frameMaxDims[4] = {H5S_UNLIMITED, static_cast<hsize_t>(height_), 
                                   static_cast<hsize_t>(width_), 4};
        hsize_t frameChunk[4] = {1, static_cast<hsize_t>(height_), 
                                static_cast<hsize_t>(width_), 4};
        
        H5::DataSpace frameSpace(4, frameDims, frameMaxDims);
        H5::DSetCreatPropList frameProps;
        frameProps.setChunk(4, frameChunk);
        frameProps.setDeflate(1);  // LZ4-like compression, level 1 for speed
        
        framesDataset_ = file_.createDataSet("frames", H5::PredType::NATIVE_UINT8, 
                                             frameSpace, frameProps);
        
        // 2. Timestamps dataset: [N] int64
        hsize_t timeDims[1] = {0};
        hsize_t timeMaxDims[1] = {H5S_UNLIMITED};
        hsize_t timeChunk[1] = {1024};  // Chunk 1024 timestamps together
        
        H5::DataSpace timeSpace(1, timeDims, timeMaxDims);
        H5::DSetCreatPropList timeProps;
        timeProps.setChunk(1, timeChunk);
        timeProps.setDeflate(1);
        
        timestampsDataset_ = file_.createDataSet("timestamps", H5::PredType::NATIVE_INT64,
                                                 timeSpace, timeProps);
        
        // 3. Memory data dataset: [N, num_attributes] float32
        if (!attributeNames_.empty()) {
            hsize_t memDims[2] = {0, attributeNames_.size()};
            hsize_t memMaxDims[2] = {H5S_UNLIMITED, attributeNames_.size()};
            hsize_t memChunk[2] = {1024, attributeNames_.size()};
            
            H5::DataSpace memSpace(2, memDims, memMaxDims);
            H5::DSetCreatPropList memProps;
            memProps.setChunk(2, memChunk);
            memProps.setDeflate(1);
            
            memoryDataset_ = file_.createDataSet("memory_data", H5::PredType::NATIVE_FLOAT,
                                                 memSpace, memProps);
            
            // Store attribute names as dataset attribute
            H5::StrType strType(H5::PredType::C_S1, H5T_VARIABLE);
            hsize_t attrDims[1] = {attributeNames_.size()};
            H5::DataSpace attrSpace(1, attrDims);
            H5::Attribute attr = memoryDataset_.createAttribute("attribute_names", 
                                                               strType, attrSpace);
            
            std::vector<const char*> attrNames;
            for (const auto& name : attributeNames_) {
                attrNames.push_back(name.c_str());
            }
            attr.write(strType, attrNames.data());
        }
        
        // 4. Inputs dataset: [N, MAX_KEYS] uint8 (boolean array)
        hsize_t inputDims[2] = {0, MAX_KEYS};
        hsize_t inputMaxDims[2] = {H5S_UNLIMITED, MAX_KEYS};
        hsize_t inputChunk[2] = {1024, MAX_KEYS};
        
        H5::DataSpace inputSpace(2, inputDims, inputMaxDims);
        H5::DSetCreatPropList inputProps;
        inputProps.setChunk(2, inputChunk);
        inputProps.setDeflate(1);
        
        inputsDataset_ = file_.createDataSet("inputs", H5::PredType::NATIVE_UINT8,
                                             inputSpace, inputProps);
        
        // 5. Latencies dataset: [N, 5] float32 (frame_capture, memory_read, keystroke_capture, disk_write, total)
        hsize_t latDims[2] = {0, 5};
        hsize_t latMaxDims[2] = {H5S_UNLIMITED, 5};
        hsize_t latChunk[2] = {1024, 5};
        
        H5::DataSpace latSpace(2, latDims, latMaxDims);
        H5::DSetCreatPropList latProps;
        latProps.setChunk(2, latChunk);
        latProps.setDeflate(1);
        
        latenciesDataset_ = file_.createDataSet("latencies", H5::PredType::NATIVE_FLOAT,
                                                latSpace, latProps);
        
        spdlog::info("HDF5 datasets initialized successfully");
        
    } catch (const H5::Exception &e) {
        spdlog::error("Failed to initialize datasets: {}", e.getCDetailMsg());
        throw;
    }
}

void H5RecordingWriter::QueueFrame(H5FrameData frameData) {
    if (finalized_) {
        spdlog::warn("Cannot queue frame - writer already finalized");
        return;
    }
    queue_.push(std::move(frameData));
}

void H5RecordingWriter::WriterThread() {
    spdlog::info("H5 writer thread started");
    
    H5FrameData frameData;
    while (queue_.pop(frameData)) {
        try {
            WriteFrame(frameData);
            framesWritten_++;
        } catch (const H5::Exception &e) {
            spdlog::error("Failed to write frame {}: {}", 
                         frameData.frameNumber, e.getCDetailMsg());
        }
    }
    
    spdlog::info("H5 writer thread stopped - {} frames written", framesWritten_.load());
}

void H5RecordingWriter::WriteFrame(const H5FrameData &frameData) {
    try {
        int frameIdx = framesWritten_.load();
        
        // 1. Write frame pixels
        {
            hsize_t newSize[4] = {static_cast<hsize_t>(frameIdx + 1), 
                                 static_cast<hsize_t>(height_), 
                                 static_cast<hsize_t>(width_), 4};
            framesDataset_.extend(newSize);
            
            hsize_t offset[4] = {static_cast<hsize_t>(frameIdx), 0, 0, 0};
            hsize_t count[4] = {1, static_cast<hsize_t>(height_), 
                               static_cast<hsize_t>(width_), 4};
            
            H5::DataSpace filespace = framesDataset_.getSpace();
            filespace.selectHyperslab(H5S_SELECT_SET, count, offset);
            
            H5::DataSpace memspace(4, count);
            framesDataset_.write(frameData.pixels.data(), H5::PredType::NATIVE_UINT8,
                                memspace, filespace);
        }
        
        // 2. Write timestamp
        {
            hsize_t newSize[1] = {static_cast<hsize_t>(frameIdx + 1)};
            timestampsDataset_.extend(newSize);
            
            hsize_t offset[1] = {static_cast<hsize_t>(frameIdx)};
            hsize_t count[1] = {1};
            
            H5::DataSpace filespace = timestampsDataset_.getSpace();
            filespace.selectHyperslab(H5S_SELECT_SET, count, offset);
            
            H5::DataSpace memspace(1, count);
            timestampsDataset_.write(&frameData.timestampMs, H5::PredType::NATIVE_INT64,
                                    memspace, filespace);
        }
        
        // 3. Write memory data
        if (!attributeNames_.empty()) {
            std::vector<float> memValues;
            for (const auto& attrName : attributeNames_) {
                float value = 0.0f;
                auto it = frameData.memoryData.find(attrName);
                if (it != frameData.memoryData.end()) {
                    try {
                        value = std::stof(it->second);
                    } catch (...) {
                        value = 0.0f;
                    }
                }
                memValues.push_back(value);
            }
            
            hsize_t newSize[2] = {static_cast<hsize_t>(frameIdx + 1), attributeNames_.size()};
            memoryDataset_.extend(newSize);
            
            hsize_t offset[2] = {static_cast<hsize_t>(frameIdx), 0};
            hsize_t count[2] = {1, attributeNames_.size()};
            
            H5::DataSpace filespace = memoryDataset_.getSpace();
            filespace.selectHyperslab(H5S_SELECT_SET, count, offset);
            
            H5::DataSpace memspace(2, count);
            memoryDataset_.write(memValues.data(), H5::PredType::NATIVE_FLOAT,
                                memspace, filespace);
        }
        
        // 4. Write inputs (encoded as boolean array)
        {
            std::vector<int> encodedKeys = EncodeKeysPressed(frameData.keysPressed);
            std::vector<uint8_t> inputArray(MAX_KEYS, 0);
            for (int keyIdx : encodedKeys) {
                if (keyIdx >= 0 && keyIdx < MAX_KEYS) {
                    inputArray[keyIdx] = 1;
                }
            }
            
            hsize_t newSize[2] = {static_cast<hsize_t>(frameIdx + 1), MAX_KEYS};
            inputsDataset_.extend(newSize);
            
            hsize_t offset[2] = {static_cast<hsize_t>(frameIdx), 0};
            hsize_t count[2] = {1, MAX_KEYS};
            
            H5::DataSpace filespace = inputsDataset_.getSpace();
            filespace.selectHyperslab(H5S_SELECT_SET, count, offset);
            
            H5::DataSpace memspace(2, count);
            inputsDataset_.write(inputArray.data(), H5::PredType::NATIVE_UINT8,
                                memspace, filespace);
        }
        
        // 5. Write latencies
        {
            float latencies[5] = {
                static_cast<float>(frameData.frameCaptureMs),
                static_cast<float>(frameData.memoryReadMs),
                static_cast<float>(frameData.keystrokeCaptureMs),
                static_cast<float>(frameData.diskWriteMs),
                static_cast<float>(frameData.totalLatencyMs)
            };
            
            hsize_t newSize[2] = {static_cast<hsize_t>(frameIdx + 1), 5};
            latenciesDataset_.extend(newSize);
            
            hsize_t offset[2] = {static_cast<hsize_t>(frameIdx), 0};
            hsize_t count[2] = {1, 5};
            
            H5::DataSpace filespace = latenciesDataset_.getSpace();
            filespace.selectHyperslab(H5S_SELECT_SET, count, offset);
            
            H5::DataSpace memspace(2, count);
            latenciesDataset_.write(latencies, H5::PredType::NATIVE_FLOAT,
                                   memspace, filespace);
        }
        
    } catch (const H5::Exception &e) {
        spdlog::error("HDF5 write error: {}", e.getCDetailMsg());
        throw;
    }
}

std::vector<int> H5RecordingWriter::EncodeKeysPressed(const std::vector<std::string> &keys) {
    std::vector<int> encoded;
    
    for (const auto& key : keys) {
        // Check if we've seen this key before
        auto it = keyMap_.find(key);
        if (it != keyMap_.end()) {
            encoded.push_back(it->second);
        } else {
            // Assign new index
            int newIdx = keyMap_.size();
            if (newIdx < MAX_KEYS) {
                keyMap_[key] = newIdx;
                encoded.push_back(newIdx);
            }
        }
    }
    
    return encoded;
}

void H5RecordingWriter::Finalize() {
    if (finalized_) {
        return;
    }
    
    spdlog::info("Finalizing H5 writer - queue size: {}", queue_.size());
    
    // Stop queue and wait for writer thread to drain
    queue_.stop();
    
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    
    // Save key mapping as dataset attribute
    if (!keyMap_.empty()) {
        try {
            H5::StrType strType(H5::PredType::C_S1, H5T_VARIABLE);
            hsize_t attrDims[1] = {keyMap_.size()};
            H5::DataSpace attrSpace(1, attrDims);
            H5::Attribute keyAttr = inputsDataset_.createAttribute("key_mapping", 
                                                                  strType, attrSpace);
            
            std::vector<std::pair<std::string, int>> sortedKeys(keyMap_.begin(), keyMap_.end());
            std::sort(sortedKeys.begin(), sortedKeys.end(), 
                     [](const auto& a, const auto& b) { return a.second < b.second; });
            
            std::vector<const char*> keyNames;
            for (const auto& pair : sortedKeys) {
                keyNames.push_back(pair.first.c_str());
            }
            keyAttr.write(strType, keyNames.data());
        } catch (const H5::Exception &e) {
            spdlog::error("Failed to write key mapping: {}", e.getCDetailMsg());
        }
    }
    
    // Close file
    try {
        framesDataset_.close();
        timestampsDataset_.close();
        if (memoryDataset_.getId() > 0) memoryDataset_.close();
        inputsDataset_.close();
        latenciesDataset_.close();
        file_.close();
    } catch (const H5::Exception &e) {
        spdlog::error("Error closing HDF5 file: {}", e.getCDetailMsg());
    }
    
    finalized_ = true;
    spdlog::info("H5RecordingWriter finalized - total frames: {}", framesWritten_.load());
}

