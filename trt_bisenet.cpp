#include "mat_transform.hpp"
#include "trt_bisenet.h"
#include "gpu_func.cuh"

BiSeNet::BiSeNet(const OnnxInitParam& params) : _params(params)
{
	cudaSetDevice(params.gpu_id);

	cudaStreamCreate(&stream_);

	Initial();
}

BiSeNet::~BiSeNet()
{
	cudaStreamSynchronize(stream_);
	if(stream_)
		cudaStreamDestroy(stream_);
	if (_context != nullptr)
		_context->destroy();
	if (_engine != nullptr)
		_engine->destroy();
	if (_runtime != nullptr)
		_runtime->destroy();
	if (h_input_tensor_ != nullptr)
		/*free(h_input_tensor_);*/
		cudaFreeHost(h_input_tensor_);
	if (h_output_tensor_ != nullptr)
		cudaFreeHost(h_output_tensor_);
	if (d_input_tensor_ != nullptr)
		cudaFree(d_input_tensor_);
	if (d_output_tensor_ != nullptr)
		cudaFree(d_output_tensor_);

	buffer_queue_.clear();
}

void BiSeNet::Initial()
{
	if (CheckFileExist(_params.rt_stream_path + _params.rt_model_name))
	{
		std::cout << "read rt model..." << std::endl;
		LoadGieStreamBuildContext(_params.rt_stream_path + _params.rt_model_name);
	}
	else
		LoadOnnxModel();
}

void BiSeNet::LoadOnnxModel()
{
	if (!CheckFileExist(_params.onnx_model_path))
	{
		std::cerr << "onnx file is not found " << _params.onnx_model_path << std::endl;
		exit(0);
	}

	nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(logger);
	assert(builder != nullptr);

	const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
	nvinfer1::INetworkDefinition* network = builder->createNetworkV2(explicitBatch);

	nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, logger);
	assert(parser->parseFromFile(_params.onnx_model_path.c_str(), 2));

	nvinfer1::IBuilderConfig* build_config = builder->createBuilderConfig();
	nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
	nvinfer1::ITensor* input = network->getInput(0);
	nvinfer1::Dims input_dims = input->getDimensions();
	std::cout << "batch_size: " << input_dims.d[0]
		<< " channels: " << input_dims.d[1]
		<< " height: " << input_dims.d[2]
		<< " width: " << input_dims.d[3] << std::endl;

	{
		profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims4{ 1, input_dims.d[1], 1, 1 });
		profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims4{ 1, input_dims.d[1], 640, 640 });
		profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims4{ 1, input_dims.d[1], 640, 640 });
		build_config->addOptimizationProfile(profile);
	}

	build_config->setMaxWorkspaceSize(1 << 30);
	if (_params.use_fp16)
	{
		if (builder->platformHasFastFp16())
		{
			builder->setHalf2Mode(true);
			std::cout << "useFP16 : " << true << std::endl;
		}
	}
	else
		std::cout << "Using GPU FP32 !" << std::endl;

	nvinfer1::ICudaEngine* engine = builder->buildEngineWithConfig(*network, *build_config);
	assert(engine != nullptr);

	nvinfer1::IHostMemory* gie_model_stream = engine->serialize();
	SaveRTModel(gie_model_stream, _params.rt_stream_path + _params.rt_model_name);

	deserializeCudaEngine(gie_model_stream->data(), gie_model_stream->size());

	builder->destroy();
	network->destroy();
	parser->destroy();
	build_config->destroy();
	engine->destroy();
}

void BiSeNet::LoadGieStreamBuildContext(const std::string& gie_file)
{
	std::ifstream fgie(gie_file, std::ios_base::in | std::ios_base::binary);
	if (!fgie)
	{
		std::cerr << "Can't read rt model from " << gie_file << std::endl;
		return;
	}

	std::stringstream buffer;
	buffer << fgie.rdbuf();

	std::string stream_model(buffer.str());

	deserializeCudaEngine(stream_model.data(), stream_model.size());
}

void BiSeNet::SaveRTModel(nvinfer1::IHostMemory* gie_model_stream, const std::string& path)
{
	std::ofstream outfile(path, std::ios_base::out | std::ios_base::binary);
	outfile.write((const char*)gie_model_stream->data(), gie_model_stream->size());
	outfile.close();
}

void BiSeNet::deserializeCudaEngine(const void* blob_data, std::size_t size)
{
	_runtime = nvinfer1::createInferRuntime(logger);
	assert(_runtime != nullptr);
	_engine = _runtime->deserializeCudaEngine(blob_data, size, nullptr);
	assert(_engine != nullptr);

	_context = _engine->createExecutionContext();
	assert(_context != nullptr);

	mallocInputOutput();
}

void BiSeNet::mallocInputOutput()
{
	int in_counts = _params.max_shape.count();
	cudaHostAlloc((void**)&h_input_tensor_, in_counts * sizeof(float), cudaHostAllocDefault);
	cudaMalloc((void**)&d_input_tensor_, in_counts * sizeof(float));

	int out_counts = _params.max_shape.num() * _params.num_classes * 
					 _params.max_shape.height() * _params.max_shape.width();
	cudaHostAlloc((void**)&h_output_tensor_, out_counts * sizeof(float), cudaHostAllocDefault);
	cudaMalloc((void**)&d_output_tensor_, out_counts * sizeof(float));

	buffer_queue_.push_back(d_input_tensor_);
	buffer_queue_.push_back(d_output_tensor_); 
}

// ====================================================== FOR INFERENCE ======================================================

cv::Mat BiSeNet::Extract(const cv::Mat& img)
{
	if (img.empty())
		return img;

	PreProcessCpu(img);
	Forward();

	/*cv::Mat res = PostProcessCpu();*/
	cv::Mat res = PostProcessGpu();
	return std::move(res);
}

void BiSeNet::PreProcessCpu(const cv::Mat& img)
{
	cv::Mat img_tmp = img;

	ComposeMatLambda compose({
		LetterResize(cv::Size(640, 640), cv::Scalar(114, 114, 114), 32),
		MatDivConstant(255),
		MatNormalize(mean_, std_),
	});

	cv::Mat sample_float = compose(img_tmp);
	input_shape_t.Reshape(1, sample_float.channels(), sample_float.rows, sample_float.cols);
	output_shape_t.Reshape(1, _params.num_classes, sample_float.rows, sample_float.cols);

	Tensor2VecMat tensor_2_mat;
	std::vector<cv::Mat> channels = tensor_2_mat(h_input_tensor_, input_shape_t); 
	cv::split(sample_float, channels);
}

void BiSeNet::Forward()
{
	cudaMemcpy(d_input_tensor_, h_input_tensor_,
		input_shape_t.count() * sizeof(float),
		cudaMemcpyHostToDevice); 

	nvinfer1::Dims4 input_dims{ 1, input_shape_t.channels(), input_shape_t.height(), input_shape_t.width() }; 
	_context->setBindingDimensions(0, input_dims);
	_context->enqueueV2(buffer_queue_.data(), stream_, nullptr);

	cudaStreamSynchronize(stream_);
}

cv::Mat BiSeNet::PostProcessCpu()
{
	int num = output_shape_t.num();
	int channels = output_shape_t.channels();
	int height = output_shape_t.height();
	int width = output_shape_t.width();
	int count = output_shape_t.count();

	cudaMemcpy(h_output_tensor_, d_output_tensor_,
		count * sizeof(float), cudaMemcpyDeviceToHost); // num * channels * height * width

	cv::Mat res = cv::Mat::zeros(height, width, CV_8UC1);
	for (int row = 0; row < height; row++)
	{
		for (int col = 0; col < width; col++)
		{
			vector<float> vec;
			for (int c = 0; c < channels; c++)
			{
				int index = row * width + col + c * height * width;
				float val = h_output_tensor_[index];
				vec.push_back(val);
			}

			int idx = findMaxIdx(vec);
			if (idx == -1)
				continue;
			res.at<uchar>(row, col) = uchar(idx);
		}
	}

	return std::move(res);
}

cv::Mat BiSeNet::PostProcessGpu()
{
	int num = output_shape_t.num();
	int channels = output_shape_t.channels();
	int height = output_shape_t.height();
	int width = output_shape_t.width();

	unsigned char* cpu_dst;
	cudaHostAlloc((void**)&cpu_dst, height * width * sizeof(float), cudaHostAllocDefault);  
	segmentation(d_output_tensor_, channels, height, width, cpu_dst);

	cv::Mat res = cv::Mat(height, width, CV_8UC1, cpu_dst);

	cudaFreeHost(cpu_dst);
	return std::move(res);
}

void BiSeNet::softmax(vector<float>& vec)
{
	float tol = 0.0;
	for (int i = 0; i < vec.size(); i++)
	{
		vec[i] = exp(vec[i]);
		tol += vec[i];
	}

	for (int i = 0; i < vec.size(); i++)
		vec[i] = vec[i] / tol;
}

int BiSeNet::findMaxIdx(const vector<float>& vec)
{
	if (vec.empty())
		return -1;
	auto pos = max_element(vec.begin(), vec.end());
	return std::distance(vec.begin(), pos);
}