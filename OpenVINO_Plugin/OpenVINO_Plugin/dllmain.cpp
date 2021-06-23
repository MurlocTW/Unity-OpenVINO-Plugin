// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

using namespace InferenceEngine;

// Create a macro to quickly mark a function for export
#define DLLExport __declspec (dllexport)

// Wrap code to prevent name-mangling issues
extern "C" {

    // The name of the current compute device
    std::string currentDevice = "";
    // List of available compute devices
    std::vector<std::string> availableDevices;
    // An unparsed list of available compute devices
    std::string allDevices = "";
    //
    std::string firstInputName;
    // The name of the output layer of Neural Network "140"
    std::string firstOutputName;

    // Stores the pixel data for model input image and output image
    cv::Mat texture;

    // Inference engine instance
    Core ie;
    // Contains all the information about the Neural Network topology and related constant values for the model
    CNNNetwork network;
    // Provides an interface for an executable network on the compute device
    ExecutableNetwork executable_network;
    // Provides an interface for an asynchronous inference request
    InferRequest infer_request;

    // Returns an unparsed list of available compute devices
    DLLExport const std::string* GetAvailableDevices() {
        return &allDevices;
    }

    // Configure the cache directory for GPU compute devices
    DLLExport void SetDeviceCache() {
        std::regex e("(GPU)(.*)");
        // Iterate through the availabel compute devices
        for (auto&& device : availableDevices) {
            // Only configure the cache directory for GPUs
            if (std::regex_match(device, e)) {
                ie.SetConfig({ {CONFIG_KEY(CACHE_DIR), "cache"} }, device);
            }
        }
    }

    // Get the names of the input and output layers and set the precision
    DLLExport void PrepareBlobs() {
        // Get information about the network input
        InputsDataMap inputInfo(network.getInputsInfo());
        firstInputName = inputInfo.begin()->first;
        inputInfo.begin()->second->setPrecision(Precision::FP32);

        // Get information about the network output
        OutputsDataMap outputInfo(network.getOutputsInfo());
        // Get the name of the output layer
        firstOutputName = outputInfo.begin()->first;
        // Set the output precision
        outputInfo.begin()->second->setPrecision(Precision::FP32);
    }

    // Set up OpenVINO inference engine
    DLLExport void InitializeOpenVINO(char* modelPath) {
        
        // Read network file
        network = ie.ReadNetwork(modelPath);
        // Set batch size to one image
        network.setBatchSize(1);
        // Get the output name and set the output precision
        PrepareBlobs();
        // Get a list of the available compute devices
        availableDevices = ie.GetAvailableDevices();
        // Reverse the order of the list
        std::reverse(availableDevices.begin(), availableDevices.end());
        // Add all available compute devices to a single string
        for (auto&& device : availableDevices) {
            allDevices += device;
            allDevices += ((device == availableDevices[availableDevices.size() - 1]) ? "" : ",");
        }
        // Specify the cache directory for GPU inference
        SetDeviceCache();
    }

    // Manually set the input resolution for the model
    DLLExport void SetInputDims(int width, int height) {
        
        // ------------- 1. Collect the map of input names and shapes from IR---------------
        auto input_shapes = network.getInputShapes();
        // ---------------------------------------------------------------------------------

         // ------------- 2. Set new input shapes -------------------------------------------
        std::string input_name;
        InferenceEngine::SizeVector input_shape;
        std::tie(input_name, input_shape) = *input_shapes.begin(); // let's consider first input only
        input_shape[0] = 1; // set batch size to the first input dimension
        input_shape[2] = height; // changes input height to the image one
        input_shape[3] = width; // changes input width to the image one
        input_shapes[input_name] = input_shape;
        // ---------------------------------------------------------------------------------

        // ------------- 3. Call reshape ---------------------------------------------------
        // Perform shape inference with the new input dimensions
        network.reshape(input_shapes);
        // Initialize the texture variable with the new dimensions
        texture = cv::Mat(height, width, CV_8UC4);
        // ---------------------------------------------------------------------------------
    }

    // Create an executable network for the target compute device
    DLLExport std::string* UploadModelToDevice(int deviceNum) {

        // Get the name for the selected device index
        currentDevice = availableDevices[deviceNum];
        // Create executable network
        executable_network = ie.LoadNetwork(network, currentDevice);
        // Create an inference request object
        infer_request = executable_network.CreateInferRequest();
        // Return the name of the current compute device
        return &currentDevice;
    }

    // Prepare input values for the model
    void PrepareInput() {
        
        // Get a poiner to the input tensor for the model
        MemoryBlob::Ptr minput = as<MemoryBlob>(infer_request.GetBlob(firstInputName));

        // locked memory holder should be alive all time while access to its buffer happens
        auto ilmHolder = minput->wmap();

        // Get the number of color channels 
        size_t num_channels = minput->getTensorDesc().getDims()[1];
        // Get the number of pixels in the input image
        size_t H = minput->getTensorDesc().getDims()[2];
        size_t W = minput->getTensorDesc().getDims()[3];
        size_t nPixels = W * H;

        // Filling input tensor with image data
        auto data = ilmHolder.as<PrecisionTrait<Precision::FP32>::value_type*>();

        // Iterate over each color channel for each pixel in image
        for (size_t pixel = 0; pixel < nPixels; pixel++) {
            /** Iterate over all channels **/
            for (size_t ch = 0; ch < num_channels; ++ch) {
                // [channels stride + pixel id ] all in bytes
                data[ch * nPixels + pixel] = texture.data[pixel * num_channels + ch];
            }
        }
    }

    // Process the raw output from the model
    void ProcessOutput() {
        
        MemoryBlob::CPtr moutput = as<MemoryBlob>(infer_request.GetBlob(firstOutputName));

        // locked memory holder should be alive all time while access to its buffer happens
        auto lmoHolder = moutput->rmap();
        const auto output_data = lmoHolder.as<const PrecisionTrait<Precision::FP32>::value_type*>();

        // Get the number of color channels 
        size_t num_channels = moutput->getTensorDesc().getDims()[1];
        // Get the number of pixels in the input image
        size_t H = moutput->getTensorDesc().getDims()[2];
        size_t W = moutput->getTensorDesc().getDims()[3];
        size_t nPixels = W * H;

        // Create a temporary vector for processing the raw model output
        std::vector<float> data_img(nPixels * num_channels);

        // Iterate through each pixel in the model output
        for (size_t i = 0; i < nPixels; i++) {

            // Get values from the model output
            data_img[i * num_channels] = static_cast<float>(output_data[i]);
            data_img[i * num_channels + 1] = static_cast<float>(output_data[(i + nPixels)]);
            data_img[i * num_channels + 2] = static_cast<float>(output_data[(i + 2 * nPixels)]);

            // Clamp color values to the range [0, 255]
            if (data_img[i * num_channels] < 0) data_img[i * num_channels] = 0;
            if (data_img[i * num_channels] > 255) data_img[i * num_channels] = 255;

            if (data_img[i * num_channels + 1] < 0) data_img[i * num_channels + 1] = 0;
            if (data_img[i * num_channels + 1] > 255) data_img[i * num_channels + 1] = 255;

            if (data_img[i * num_channels + 2] < 0) data_img[i * num_channels + 2] = 0;
            if (data_img[i * num_channels + 2] > 255) data_img[i * num_channels + 2] = 255;

            // Copy the processed output to the OpenCV Mat
            texture.data[i * num_channels] = data_img[i * num_channels];
            texture.data[i * num_channels + 1] = data_img[i * num_channels + 1];
            texture.data[i * num_channels + 2] = data_img[i * num_channels + 2];
        }
    }
       
    // Perform inference with the provided texture data
    DLLExport void PerformInference(uchar* inputData) {

        // Assign the inputData to the OpenCV Mat
        texture.data = inputData;
        // Remove the alpha channel
        cv::cvtColor(texture, texture, cv::COLOR_RGBA2RGB);
        
        PrepareInput();

        // Perform inference
        infer_request.Infer();

        ProcessOutput();

        // Add alpha channel
        cv::cvtColor(texture, texture, cv::COLOR_RGB2RGBA);
        // Copy values form the OpenCV Mat back to inputData
        std::memcpy(inputData, texture.data, texture.total() * 4);
    }
}