//
// caffe_.cpp provides wrappers of the caffe::Solver class, caffe::Net class,
// caffe::Layer class and caffe::Blob class and some caffe::Caffe functions,
// so that one could easily use Caffe from matlab.
// Note that for matlab, we will simply use float as the data type.

// Internally, data is stored with dimensions reversed from Caffe's:
// e.g., if the Caffe blob axes are (num, channels, height, width),
// the matcaffe data is stored as (width, height, channels, num)
// where width is the fastest dimension.

#include <sstream>
#include <string>
#include <vector>

#include "mex.h"

#include "caffe/caffe.hpp"
#include "boost/algorithm/string.hpp"

#define MEX_ARGS int nlhs, mxArray **plhs, int nrhs, const mxArray **prhs

using namespace caffe;  // NOLINT(build/namespaces)

// Do CHECK and throw a Mex error if check fails
inline void mxCHECK(bool expr, const char* msg) {
  if (!expr) {
    mexErrMsgTxt(msg);
  }
}
inline void mxERROR(const char* msg) { mexErrMsgTxt(msg); }

// Check if a file exists and can be opened
void mxCHECK_FILE_EXIST(const char* file) {
  std::ifstream f(file);
  if (!f.good()) {
    f.close();
    std::string msg("Could not open file ");
    msg += file;
    mxERROR(msg.c_str());
  }
  f.close();
}

// The pointers to caffe::Solver and caffe::Net instances
static vector<shared_ptr<Solver<float> > > solvers_;
static vector<shared_ptr<Net<float> > > nets_;
//for multi-gpu
static vector<P2PSync<float>* > syncs_;
// init_key is generated at the beginning and everytime you call reset
static double init_key = static_cast<double>(caffe_rng_rand());
//static double init_key = -2;

/** -----------------------------------------------------------------
** data conversion functions
**/
// Enum indicates which blob memory to use
enum WhichMemory { DATA, DIFF };

// Copy matlab array to Blob data or diff
static void mx_mat_to_blob(const mxArray* mx_mat, Blob<float>* blob,
  WhichMemory data_or_diff) {
  mxCHECK(blob->count() == mxGetNumberOfElements(mx_mat),
    "number of elements in target blob doesn't match that in input mxArray");
  const float* mat_mem_ptr = reinterpret_cast<const float*>(mxGetData(mx_mat));
  float* blob_mem_ptr = NULL;
  switch (Caffe::mode()) {
  case Caffe::CPU:
    blob_mem_ptr = (data_or_diff == DATA ?
      blob->mutable_cpu_data() : blob->mutable_cpu_diff());
    break;
  case Caffe::GPU:
    blob_mem_ptr = (data_or_diff == DATA ?
      blob->mutable_gpu_data() : blob->mutable_gpu_diff());
    break;
  default:
    mxERROR("Unknown Caffe mode");
  }
  caffe_copy(blob->count(), mat_mem_ptr, blob_mem_ptr);
}

// Copy Blob data or diff to matlab array
static mxArray* blob_to_mx_mat(const Blob<float>* blob,
  WhichMemory data_or_diff) {
  const int num_axes = blob->num_axes();
  vector<mwSize> dims(num_axes);
  for (int blob_axis = 0, mat_axis = num_axes - 1; blob_axis < num_axes;
    ++blob_axis, --mat_axis) {
    dims[mat_axis] = static_cast<mwSize>(blob->shape(blob_axis));
  }
  // matlab array needs to have at least one dimension, convert scalar to 1-dim
  if (num_axes == 0) {
    dims.push_back(1);
  }
  mxArray* mx_mat =
    mxCreateNumericArray(dims.size(), dims.data(), mxSINGLE_CLASS, mxREAL);
  float* mat_mem_ptr = reinterpret_cast<float*>(mxGetData(mx_mat));
  const float* blob_mem_ptr = NULL;
  switch (Caffe::mode()) {
  case Caffe::CPU:
    blob_mem_ptr = (data_or_diff == DATA ? blob->cpu_data() : blob->cpu_diff());
    break;
  case Caffe::GPU:
    blob_mem_ptr = (data_or_diff == DATA ? blob->gpu_data() : blob->gpu_diff());
    break;
  default:
    mxERROR("Unknown Caffe mode");
  }
  caffe_copy(blob->count(), blob_mem_ptr, mat_mem_ptr);
  return mx_mat;
}

// Convert vector<int> to matlab row vector
static mxArray* int_vec_to_mx_vec(const vector<int>& int_vec) {
  mxArray* mx_vec = mxCreateDoubleMatrix(int_vec.size(), 1, mxREAL);
  double* vec_mem_ptr = mxGetPr(mx_vec);
  for (int i = 0; i < int_vec.size(); i++) {
    vec_mem_ptr[i] = static_cast<double>(int_vec[i]);
  }
  return mx_vec;
}

// Convert vector<string> to matlab cell vector of strings
static mxArray* str_vec_to_mx_strcell(const vector<std::string>& str_vec) {
  mxArray* mx_strcell = mxCreateCellMatrix(str_vec.size(), 1);
  for (int i = 0; i < str_vec.size(); i++) {
    mxSetCell(mx_strcell, i, mxCreateString(str_vec[i].c_str()));
  }
  return mx_strcell;
}

/** -----------------------------------------------------------------
** handle and pointer conversion functions
** a handle is a struct array with the following fields
**   (uint64) ptr      : the pointer to the C++ object
**   (double) init_key : caffe initialization key
**/
// Convert a handle in matlab to a pointer in C++. Check if init_key matches
template <typename T>
static T* handle_to_ptr(const mxArray* mx_handle) {
  mxArray* mx_ptr = mxGetField(mx_handle, 0, "ptr");
  mxArray* mx_init_key = mxGetField(mx_handle, 0, "init_key");
  mxCHECK(mxIsUint64(mx_ptr), "pointer type must be uint64");
  mxCHECK(mxGetScalar(mx_init_key) == init_key,
    "Could not convert handle to pointer due to invalid init_key. "
    "The object might have been cleared.");
  return reinterpret_cast<T*>(*reinterpret_cast<uint64_t*>(mxGetData(mx_ptr)));
}

// Create a handle struct vector, without setting up each handle in it
template <typename T>
static mxArray* create_handle_vec(int ptr_num) {
  const int handle_field_num = 2;
  const char* handle_fields[handle_field_num] = { "ptr", "init_key" };
  return mxCreateStructMatrix(ptr_num, 1, handle_field_num, handle_fields);
}

// Set up a handle in a handle struct vector by its index
template <typename T>
static void setup_handle(const T* ptr, int index, mxArray* mx_handle_vec) {
  mxArray* mx_ptr = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
  *reinterpret_cast<uint64_t*>(mxGetData(mx_ptr)) =
    reinterpret_cast<uint64_t>(ptr);
  mxSetField(mx_handle_vec, index, "ptr", mx_ptr);
  mxSetField(mx_handle_vec, index, "init_key", mxCreateDoubleScalar(init_key));
}

// Convert a pointer in C++ to a handle in matlab
template <typename T>
static mxArray* ptr_to_handle(const T* ptr) {
  mxArray* mx_handle = create_handle_vec<T>(1);
  setup_handle(ptr, 0, mx_handle);
  return mx_handle;
}

// Convert a vector of shared_ptr in C++ to handle struct vector
template <typename T>
static mxArray* ptr_vec_to_handle_vec(const vector<shared_ptr<T> >& ptr_vec) {
  mxArray* mx_handle_vec = create_handle_vec<T>(ptr_vec.size());
  for (int i = 0; i < ptr_vec.size(); i++) {
    setup_handle(ptr_vec[i].get(), i, mx_handle_vec);
  }
  return mx_handle_vec;
}

/** -----------------------------------------------------------------
** matlab command functions: caffe_(api_command, arg1, arg2, ...)
**/
// Usage: caffe_('get_solver', solver_file);
static void get_solver(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsChar(prhs[0]),
    "Usage: caffe_('get_solver', solver_file)");
  char* solver_file = mxArrayToString(prhs[0]);
  mxCHECK_FILE_EXIST(solver_file);
  try
  {
	  SolverParameter solver_param;
	  ReadSolverParamsFromTextFileOrDie(solver_file, &solver_param);
	  shared_ptr<Solver<float> > solver(
		  SolverRegistry<float>::CreateSolver(solver_param));
	  solvers_.push_back(solver);
	  plhs[0] = ptr_to_handle<Solver<float> >(solver.get());
  }
  catch (...)
  {
	  mxFree(solver_file);
	  mxERROR("CAFFE_mex: get_solver exploded...");
	  return;
  }
  mxFree(solver_file);
}

// Known issues:
// step number should never exceed max_iter(set in solver .prototxt), or deadlock will happen
//vector<int> gpus;
//20160215find
caffe::P2PSync<float> *sync_ptr = NULL;
static void get_solver_multigpu(MEX_ARGS) {
	mexPrintf("======== USING MULTI-GPU SOLVER ========\n");
	mxCHECK(mxIsChar(prhs[0]),
		"Usage: caffe_('get_solver_multigpu', solver_file, [snapshot file], [gpus to use])");
	
	char* solver_file = mxArrayToString(prhs[0]);
	mxCHECK_FILE_EXIST(solver_file);

	char* snapshot_file = NULL;
	double* data_ptr;
	int ele_num;
	if (nrhs == 1) {
	}
	else if (nrhs == 2) {
		if (mxIsChar(prhs[1])) {
			snapshot_file = mxArrayToString(prhs[1]);
			mxCHECK_FILE_EXIST(snapshot_file);
		}
		else if (mxIsDouble(prhs[1])) {
			data_ptr = reinterpret_cast<double*>(mxGetPr(prhs[1]));
			ele_num = mxGetNumberOfElements(prhs[1]);
		}
		else {
			mxERROR("get_solver_multigpu: Unrecognized parameter!!!");
		}
	}
	else if (nrhs == 3) {
		if (!(mxIsChar(prhs[1]) && mxIsDouble(prhs[2]))) {
			mxERROR("get_solver_multigpu: Unrecognized parameter!!!");
		}
		snapshot_file = mxArrayToString(prhs[1]);
		mxCHECK_FILE_EXIST(snapshot_file);

		data_ptr = reinterpret_cast<double*>(mxGetPr(prhs[2]));
		ele_num = mxGetNumberOfElements(prhs[2]);
	}
	else {
		mxERROR("get_solver_multigpu: Wrong input parameter number!!!");
	}


	SolverParameter solver_param;
	ReadSolverParamsFromTextFileOrDie(solver_file, &solver_param);

	// ====== SET UP GPUs ======
	vector<int> gpus;
	gpus.clear();

	int count = 0;
#ifndef CPU_ONLY
	CUDA_CHECK(cudaGetDeviceCount(&count));
#else
	NO_GPU;
#endif
	for (int i = 0; i < ele_num; ++i)
	{
		int device_id = int(data_ptr[i]);
		if (device_id >= count)
			mexErrMsgTxt("get_solver_multigpu: device_id should in [0, gpuDeviceCount-1]");
		gpus.push_back(device_id);
	}
	//=====================================================================
	//for (int i = 0; i < count; ++i) {
	//	gpus.push_back(i);
	//}

	if (gpus.size() == 0) {
		mxERROR("No GPU found!!!\n");
	}
	else {
		ostringstream s;
		for (int i = 0; i < gpus.size(); ++i) {
			s << (i ? ", " : "") << gpus[i];
		}
		LOG(INFO) << "Using GPUs " << s.str();
		mexPrintf("Using GPUs %s\n", s.str());

		solver_param.set_device_id(gpus[0]);
		Caffe::SetDevice(gpus[0]);
		Caffe::set_mode(Caffe::GPU);
		Caffe::set_solver_count(gpus.size());
	}

	// ==========================
	try
	{
		shared_ptr<Solver<float> > solver(
			SolverRegistry<float>::CreateSolver(solver_param));
		
		if (snapshot_file != NULL) {
			mexPrintf("Resuming from %s\n", snapshot_file);
			LOG(INFO) << "Resuming from " << snapshot_file;
			if (boost::ends_with(snapshot_file, ".solverstate")) {
				mexPrintf("It is a SolverState file......\n");
				solver->Restore(snapshot_file);
			}
			else if (boost::ends_with(snapshot_file, ".caffemodel")) {
				mexPrintf("It is a CaffeModel file......\n");
				solver->net()->CopyTrainedLayersFrom(snapshot_file);
			}
			else {
				mxERROR("Only support *.solverstate *.caffemodel, please rename your input binary file");
			}
		}

		sync_ptr = new caffe::P2PSync<float>(solver, NULL, solver->param());
		//caffe::P2PSync<float> sync(solver, NULL, solver->param());
		//syncs_.push_back(&sync);
		solvers_.push_back(solver);
		plhs[0] = ptr_to_handle<Solver<float> >(solver.get());

		//syncs_[0]->run(gpus);
		//sync_ptr->run(gpus);
		mexPrintf("Building GPU tree......\n");
		sync_ptr->init_syncs(gpus);
	}
	catch (...)
	{
		mxFree(solver_file);
		mxERROR("CAFFE_mex: get_solver_multigpu exploded...");
		return;
	}
	mxFree(solver_file);
}

// Usage: caffe_('solver_get_attr', hSolver)
static void solver_get_attr(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('solver_get_attr', hSolver)");
  Solver<float>* solver = handle_to_ptr<Solver<float> >(prhs[0]);
  const int solver_attr_num = 2;
  const char* solver_attrs[solver_attr_num] = { "hNet_net", "hNet_test_nets" };
  mxArray* mx_solver_attr = mxCreateStructMatrix(1, 1, solver_attr_num,
    solver_attrs);
  mxSetField(mx_solver_attr, 0, "hNet_net",
    ptr_to_handle<Net<float> >(solver->net().get()));
  mxSetField(mx_solver_attr, 0, "hNet_test_nets",
    ptr_vec_to_handle_vec<Net<float> >(solver->test_nets()));
  plhs[0] = mx_solver_attr;
}

// Usage: caffe_('solver_get_iter', hSolver)
static void solver_get_iter(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('solver_get_iter', hSolver)");
  Solver<float>* solver = handle_to_ptr<Solver<float> >(prhs[0]);
  plhs[0] = mxCreateDoubleScalar(solver->iter());
}

// Usage: caffe_('solver_restore', hSolver, snapshot_file)
static void solver_restore(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsChar(prhs[1]),
    "Usage: caffe_('solver_restore', hSolver, snapshot_file)");
  Solver<float>* solver = handle_to_ptr<Solver<float> >(prhs[0]);
  char* snapshot_file = mxArrayToString(prhs[1]);
  mxCHECK_FILE_EXIST(snapshot_file);
  solver->Restore(snapshot_file);
  mxFree(snapshot_file);
}

// Usage: caffe_('solver_solve', hSolver)
static void solver_solve(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('solver_solve', hSolver)");
  Solver<float>* solver = handle_to_ptr<Solver<float> >(prhs[0]);
  solver->Solve();
}

// Usage: caffe_('solver_step', hSolver, iters)
static void solver_step(MEX_ARGS) {
	mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsDouble(prhs[1]),
		"Usage: caffe_('solver_step', hSolver, iters)");
	Solver<float>* solver = handle_to_ptr<Solver<float> >(prhs[0]);
	int iters = mxGetScalar(prhs[1]);
	try
	{
		solver->Step(iters);
	}
	catch (...) {
		mxERROR("CAFFE_mex: solver_step exploded...");
		return;
	}
}

// ====================================================================================
// Usage: caffe_('solver_snapshot', hSolver, save_file)
//20160215find
static void solver_snapshot(MEX_ARGS) {
	mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsChar(prhs[1]),
		"Usage: caffe_('solver_snapshot', hSolver, save_file)");
	Solver<float>* solver = handle_to_ptr<Solver<float> >(prhs[0]);
	char* snapshot_file = mxArrayToString(prhs[1]);
	
	if (snapshot_file[0] == '\0') {
		solver->Snapshot();
	}
	else {
		const string *prefix = &(solver->param().snapshot_prefix());
		string ori_prefix = *prefix;
		*(string*)prefix = "@" + (string)snapshot_file;
		solver->Snapshot();
		*(string*)prefix = ori_prefix;
		mxFree(snapshot_file);
	}
}
// ====================================================================================

// Usage: caffe_('get_net', model_file, phase_name)
static void get_net(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsChar(prhs[0]) && mxIsChar(prhs[1]),
    "Usage: caffe_('get_net', model_file, phase_name)");
  char* model_file = mxArrayToString(prhs[0]);
  char* phase_name = mxArrayToString(prhs[1]);
  mxCHECK_FILE_EXIST(model_file);
  Phase phase;
  if (strcmp(phase_name, "train") == 0) {
    phase = TRAIN;
  }
  else if (strcmp(phase_name, "test") == 0) {
    phase = TEST;
  }
  else {
    mxERROR("Unknown phase");
  }
  shared_ptr<Net<float> > net(new caffe::Net<float>(model_file, phase));
  nets_.push_back(net);
  plhs[0] = ptr_to_handle<Net<float> >(net.get());
  mxFree(model_file);
  mxFree(phase_name);
}

// Usage: caffe_('net_get_attr', hNet)
static void net_get_attr(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('net_get_attr', hNet)");
  Net<float>* net = handle_to_ptr<Net<float> >(prhs[0]);
  const int net_attr_num = 6;
  const char* net_attrs[net_attr_num] = { "hLayer_layers", "hBlob_blobs",
    "input_blob_indices", "output_blob_indices", "layer_names", "blob_names" };
  mxArray* mx_net_attr = mxCreateStructMatrix(1, 1, net_attr_num,
    net_attrs);
  mxSetField(mx_net_attr, 0, "hLayer_layers",
    ptr_vec_to_handle_vec<Layer<float> >(net->layers()));
  mxSetField(mx_net_attr, 0, "hBlob_blobs",
    ptr_vec_to_handle_vec<Blob<float> >(net->blobs()));
  mxSetField(mx_net_attr, 0, "input_blob_indices",
    int_vec_to_mx_vec(net->input_blob_indices()));
  mxSetField(mx_net_attr, 0, "output_blob_indices",
    int_vec_to_mx_vec(net->output_blob_indices()));
  mxSetField(mx_net_attr, 0, "layer_names",
    str_vec_to_mx_strcell(net->layer_names()));
  mxSetField(mx_net_attr, 0, "blob_names",
    str_vec_to_mx_strcell(net->blob_names()));
  plhs[0] = mx_net_attr;
}

// Usage: caffe_('net_forward', hNet)
//20160215find
static void net_forward(MEX_ARGS) {
  mxCHECK(nrhs <= 3 && mxIsStruct(prhs[0]),
    "Usage: caffe_('net_forward', hNet, from_layer=0, to_layer=end)");
  try {

	  Net<float>* net = handle_to_ptr<Net<float> >(prhs[0]);
	  if (nrhs == 1)
		  net->ForwardPrefilled();
	  else if (nrhs == 2) {
		  mxCHECK(mxIsDouble(prhs[1]),
			  "Usage: caffe_('net_forward', hNet, from_layer=0, to_layer=end)");
		  net->ForwardFrom((int)mxGetScalar(prhs[1]));
	  }
	  else if (nrhs == 3) {
		  mxCHECK(mxIsDouble(prhs[1]) && mxIsDouble(prhs[2]),
			  "Usage: caffe_('net_forward', hNet, from_layer=0, to_layer=end)");
		  net->ForwardFromTo((int)mxGetScalar(prhs[1]), (int)mxGetScalar(prhs[2]));
	  }
  }
  catch (...) {
	  mxERROR("CAFFE_mex: net_forward exploded...");
	  return;
  }
}

// Usage: caffe_('net_backward', hNet)
//20160215find
static void net_backward(MEX_ARGS) {
  mxCHECK(nrhs <= 3 && mxIsStruct(prhs[0]),
    "Usage: caffe_('net_backward', hNet, from_layer=end, to_layer=0)");
  Net<float>* net = handle_to_ptr<Net<float> >(prhs[0]);
  if (nrhs == 1)
    net->Backward();
  else if (nrhs == 2) {
    mxCHECK(mxIsDouble(prhs[1]),
      "Usage: caffe_('net_backward', hNet, from_layer=end, to_layer=0)");
    net->BackwardFrom((int)mxGetScalar(prhs[1]));
  }
  else if (nrhs == 3) {
    mxCHECK(mxIsDouble(prhs[1]) && mxIsDouble(prhs[2]),
      "Usage: caffe_('net_backward', hNet, from_layer=end, to_layer=0)");
    net->BackwardFromTo((int)mxGetScalar(prhs[1]), (int)mxGetScalar(prhs[2]));
  }
}

// Usage: caffe_('net_copy_from', hNet, weights_file)
static void net_copy_from(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsChar(prhs[1]),
    "Usage: caffe_('net_copy_from', hNet, weights_file)");
  Net<float>* net = handle_to_ptr<Net<float> >(prhs[0]);
  char* weights_file = mxArrayToString(prhs[1]);
  mxCHECK_FILE_EXIST(weights_file);
  net->CopyTrainedLayersFrom(weights_file);
  mxFree(weights_file);
}

// Usage: caffe_('net_reshape', hNet)
static void net_reshape(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('net_reshape', hNet)");
  Net<float>* net = handle_to_ptr<Net<float> >(prhs[0]);
  net->Reshape();
}

// Usage: caffe_('net_save', hNet, save_file)
static void net_save(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsChar(prhs[1]),
    "Usage: caffe_('net_save', hNet, save_file)");
  Net<float>* net = handle_to_ptr<Net<float> >(prhs[0]);
  char* weights_file = mxArrayToString(prhs[1]);
  NetParameter net_param;
  net->ToProto(&net_param, false);
  WriteProtoToBinaryFile(net_param, weights_file);
  mxFree(weights_file);
}

// Usage: caffe_('layer_get_attr', hLayer)
static void layer_get_attr(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('layer_get_attr', hLayer)");
  Layer<float>* layer = handle_to_ptr<Layer<float> >(prhs[0]);
  const int layer_attr_num = 1;
  const char* layer_attrs[layer_attr_num] = { "hBlob_blobs" };
  mxArray* mx_layer_attr = mxCreateStructMatrix(1, 1, layer_attr_num,
    layer_attrs);
  mxSetField(mx_layer_attr, 0, "hBlob_blobs",
    ptr_vec_to_handle_vec<Blob<float> >(layer->blobs()));
  plhs[0] = mx_layer_attr;
}

// Usage: caffe_('layer_get_type', hLayer)
static void layer_get_type(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('layer_get_type', hLayer)");
  Layer<float>* layer = handle_to_ptr<Layer<float> >(prhs[0]);
  plhs[0] = mxCreateString(layer->type());
}

// Usage: caffe_('blob_get_shape', hBlob)
static void blob_get_shape(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('blob_get_shape', hBlob)");
  Blob<float>* blob = handle_to_ptr<Blob<float> >(prhs[0]);
  const int num_axes = blob->num_axes();
  mxArray* mx_shape = mxCreateDoubleMatrix(1, num_axes, mxREAL);
  double* shape_mem_mtr = mxGetPr(mx_shape);
  for (int blob_axis = 0, mat_axis = num_axes - 1; blob_axis < num_axes;
    ++blob_axis, --mat_axis) {
    shape_mem_mtr[mat_axis] = static_cast<double>(blob->shape(blob_axis));
  }
  plhs[0] = mx_shape;
}

// Usage: caffe_('blob_reshape', hBlob, new_shape)
static void blob_reshape(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsDouble(prhs[1]),
    "Usage: caffe_('blob_reshape', hBlob, new_shape)");
  Blob<float>* blob = handle_to_ptr<Blob<float> >(prhs[0]);
  const mxArray* mx_shape = prhs[1];
  double* shape_mem_mtr = mxGetPr(mx_shape);
  const int num_axes = mxGetNumberOfElements(mx_shape);
  vector<int> blob_shape(num_axes);
  for (int blob_axis = 0, mat_axis = num_axes - 1; blob_axis < num_axes;
    ++blob_axis, --mat_axis) {
    blob_shape[blob_axis] = static_cast<int>(shape_mem_mtr[mat_axis]);
  }
  blob->Reshape(blob_shape);
}

// Usage: caffe_('blob_get_data', hBlob)
static void blob_get_data(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('blob_get_data', hBlob)");
  Blob<float>* blob = handle_to_ptr<Blob<float> >(prhs[0]);
  plhs[0] = blob_to_mx_mat(blob, DATA);
}

// Usage: caffe_('blob_set_data', hBlob, new_data)
static void blob_set_data(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsSingle(prhs[1]),
    "Usage: caffe_('blob_set_data', hBlob, new_data)");
  Blob<float>* blob = handle_to_ptr<Blob<float> >(prhs[0]);
  mx_mat_to_blob(prhs[1], blob, DATA);
}
//================================================================================================
// Usage: caffe_('blob_set_data_multigpu', BlobIndex, new_data)
//20160215find
static void blob_set_data_multigpu(MEX_ARGS) {
	mxCHECK(nrhs == 2 && mxIsUint32(prhs[0]) && mxIsCell(prhs[1]),
		"Usage: caffe_('blob_set_data_multigpu', hBlob, new_multi_gpu_data_cell)");

	mxCHECK(sync_ptr != NULL,"blob_set_data_multigpu only work on multi-GPU solver");

	unsigned int blob_index = *(unsigned int*)(mxGetPr(prhs[0]));
	blob_index = blob_index - 1;
	//mexPrintf("Real Blob index: %d\n", blob_index);

	vector<shared_ptr<P2PSync<float>>>* sync_vec = sync_ptr->get_syncs();
	//mexPrintf("blob_set_data_multigpu: Number of syncs: %d\n", sync_vec->size());
	
	if (mxGetNumberOfElements(prhs[1]) != sync_vec->size())
		mexErrMsgTxt("blob_set_data_multigpu: input size should be equal to selected gpu number.\n");

	// set root solver
	const mxArray* const elem = mxGetCell(prhs[1], 0);

	mxCHECK(mxIsSingle(elem),"Input data should be single-precision float!!!\n");

	Blob<float>* blob = sync_ptr->solver()->net()->blobs()[blob_index].get();
	//mexPrintf("Size of data for each sync: %d\nSize of blob: %d\n", mxGetNumberOfElements(elem), blob->count());
	//mexPrintf("Blob index: %d\n", blob_index);
	mx_mat_to_blob(elem, blob, DATA);

	int initial_device;
	CUDA_CHECK(cudaGetDevice(&initial_device));

	for (int i = 1; i < sync_vec->size(); i++)
	{
		Solver<float> *solver = (*sync_vec)[i]->solver().get();
		Net<float> *net = solver->net().get();
		blob = net->blobs()[blob_index].get();
		//mexPrintf("sync %d: \tNumber of blobs: %d\n", i, net->blobs().size());

		const mxArray* const elem = mxGetCell(prhs[1], i);
		mxCHECK(mxIsSingle(elem), "Input data should be single-precision float!!!\n");
		//mexPrintf("Size of data for each sync: %d\nSize of blob: %d\n", mxGetNumberOfElements(elem), blob->count());
		//mexPrintf("Blob index: %d\n", blob_index);
		
		CUDA_CHECK(cudaSetDevice(solver->param().device_id()));
		mx_mat_to_blob(elem, blob, DATA);
		CUDA_CHECK(cudaSetDevice(initial_device));
		//(*sync_vec)[i]->solver_->net->blobs()[blob_index];
	}
}

// Usage: caffe_('blob_get_diff', hBlob)
static void blob_get_diff(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsStruct(prhs[0]),
    "Usage: caffe_('blob_get_diff', hBlob)");
  Blob<float>* blob = handle_to_ptr<Blob<float> >(prhs[0]);
  plhs[0] = blob_to_mx_mat(blob, DIFF);
}

// Usage: caffe_('blob_set_diff', hBlob, new_diff)
static void blob_set_diff(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsStruct(prhs[0]) && mxIsSingle(prhs[1]),
    "Usage: caffe_('blob_set_diff', hBlob, new_diff)");
  Blob<float>* blob = handle_to_ptr<Blob<float> >(prhs[0]);
  mx_mat_to_blob(prhs[1], blob, DIFF);
}

// Usage: caffe_('set_mode_cpu')
static void set_mode_cpu(MEX_ARGS) {
  mxCHECK(nrhs == 0, "Usage: caffe_('set_mode_cpu')");
  Caffe::set_mode(Caffe::CPU);
}

// Usage: caffe_('set_mode_gpu')
static void set_mode_gpu(MEX_ARGS) {
  mxCHECK(nrhs == 0, "Usage: caffe_('set_mode_gpu')");
  Caffe::set_mode(Caffe::GPU);
}

// Usage: caffe_('set_device', device_id)
static void set_device(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsDouble(prhs[0]),
    "Usage: caffe_('set_device', device_id)");
  int device_id = static_cast<int>(mxGetScalar(prhs[0]));
  Caffe::SetDevice(device_id);
}

// Usage: caffe_('get_init_key')
static void get_init_key(MEX_ARGS) {
  mxCHECK(nrhs == 0, "Usage: caffe_('get_init_key')");
  plhs[0] = mxCreateDoubleScalar(init_key);
}

// Usage: caffe_('reset')
static void reset(MEX_ARGS) {
  mxCHECK(nrhs == 0, "Usage: caffe_('reset')");
  // Clear solvers and stand-alone nets
  mexPrintf("Cleared %d solvers and %d stand-alone nets\n",
    solvers_.size()+syncs_.size(), nets_.size());
  solvers_.clear();
  syncs_.clear();
  nets_.clear();
  delete(sync_ptr);
  sync_ptr = NULL;
  // Generate new init_key, so that handles created before becomes invalid
  init_key = static_cast<double>(caffe_rng_rand());
}

// Usage: caffe_('read_mean', mean_proto_file)
static void read_mean(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsChar(prhs[0]),
    "Usage: caffe_('read_mean', mean_proto_file)");
  char* mean_proto_file = mxArrayToString(prhs[0]);
  mxCHECK_FILE_EXIST(mean_proto_file);
  Blob<float> data_mean;
  BlobProto blob_proto;
  bool result = ReadProtoFromBinaryFile(mean_proto_file, &blob_proto);
  mxCHECK(result, "Could not read your mean file");
  data_mean.FromProto(blob_proto);
  plhs[0] = blob_to_mx_mat(&data_mean, DATA);
  mxFree(mean_proto_file);
}

static bool is_log_inited = false;

static void glog_failure_handler() {
  static bool is_glog_failure = false;
  if (!is_glog_failure) {
    is_glog_failure = true;
    ::google::FlushLogFiles(0);
    mexErrMsgTxt("glog check error, please check log and clear mex");
  }
}

static void protobuf_log_handler(::google::protobuf::LogLevel level, const char* filename, int line,
  const std::string& message) {
  const int max_err_length = 512;
  char err_message[max_err_length];
  sprintf_s(err_message, max_err_length, "Protobuf : %s . at %s Line %d",
    message.c_str(), filename, line);
  LOG(INFO) << err_message;
  ::google::FlushLogFiles(0);
  mexErrMsgTxt(err_message);
}

// Usage: caffe_('init_log', log_base_filename)
static void init_log(MEX_ARGS) {
  mxCHECK(nrhs == 1 && mxIsChar(prhs[0]),
    "Usage: caffe_('init_log', log_dir)");
  if (is_log_inited)
    ::google::ShutdownGoogleLogging();
  char* log_base_filename = mxArrayToString(prhs[0]);
  ::google::SetLogDestination(0, log_base_filename);
  mxFree(log_base_filename);
  ::google::protobuf::SetLogHandler(&protobuf_log_handler);
  ::google::InitGoogleLogging("caffe_mex");
  ::google::InstallFailureFunction(&glog_failure_handler);

  is_log_inited = true;
}

void initGlog() {
  if (is_log_inited) return;
  string log_dir = ".\\log\\";
  _mkdir(log_dir.c_str());
  std::string now_time = boost::posix_time::to_iso_extended_string(boost::posix_time::second_clock::local_time());
  now_time[13] = '-';
  now_time[16] = '-';
  string log_file = log_dir + "INFO" + now_time + ".txt";
  const char* log_base_filename = log_file.c_str();
  ::google::SetLogDestination(0, log_base_filename);
  ::google::protobuf::SetLogHandler(&protobuf_log_handler);
  ::google::InitGoogleLogging("caffe_mex");
  ::google::InstallFailureFunction(&glog_failure_handler);

  is_log_inited = true;
}

// Usage: caffe_('write_mean', mean_data, mean_proto_file)
static void write_mean(MEX_ARGS) {
  mxCHECK(nrhs == 2 && mxIsSingle(prhs[0]) && mxIsChar(prhs[1]),
      "Usage: caffe_('write_mean', mean_data, mean_proto_file)");
  char* mean_proto_file = mxArrayToString(prhs[1]);
  int ndims = mxGetNumberOfDimensions(prhs[0]);
  mxCHECK(ndims >= 2 && ndims <= 3, "mean_data must have at 2 or 3 dimensions");
  const mwSize *dims = mxGetDimensions(prhs[0]);
  int width = dims[0];
  int height = dims[1];
  int channels;
  if (ndims == 3)
    channels = dims[2];
  else
    channels = 1;
  Blob<float> data_mean(1, channels, height, width);
  mx_mat_to_blob(prhs[0], &data_mean, DATA);
  BlobProto blob_proto;
  data_mean.ToProto(&blob_proto, false);
  WriteProtoToBinaryFile(blob_proto, mean_proto_file);
  mxFree(mean_proto_file);
}


/** -------------------------- Customized functions for LY ---------------------------
**
**/
// function blob_get_data_byname_multigpu
// input:  blob_name 
// output: response
static void blob_get_data_byname_multigpu(MEX_ARGS) {
	if (nrhs != 1) {
		LOG(ERROR) << "Only given " << nrhs << " arguments";
		mexErrMsgTxt("caffe_mex : Wrong number of arguments");
	}

	if (solvers_.empty())
	{
		mexPrintf("No solver inited!\n");
		plhs[0] = mxCreateDoubleScalar(-1);
		return;
	}

	mxCHECK(sync_ptr != NULL, "blob_set_data_multigpu only work on multi-GPU solver");

	vector<shared_ptr<P2PSync<float>>>* sync_vec = sync_ptr->get_syncs(); 

	char* blob_name = mxArrayToString(prhs[0]);

	mxArray* top;
	top = mxCreateCellMatrix(int(sync_vec->size()), 1);
	for (int i = 0; i < sync_vec->size(); i++)
	{
		Solver<float> *solver;
		if (i == 0)
		{
			solver = sync_ptr->solver().get();
		}
		else
		{
			solver = (*sync_vec)[i]->solver().get();
		}

		Net<float> *net = solver->net().get();
		Blob<float> *blob = net->blob_by_name(blob_name).get();

		mxArray* response = blob_to_mx_mat(blob, DATA);
		mxSetCell(top, i, response);
	}

	plhs[0] = top;
	mxFree(blob_name);
}

// Usage: caffe_('solver_teststep_multigpu')
static void solver_teststep_multigpu(MEX_ARGS) {
	mxCHECK(nrhs == 0,
		"Usage: caffe_('solver_teststep_multigpu')");
	
	vector<shared_ptr<P2PSync<float>>>* sync_vec = sync_ptr->get_syncs();

	int initial_device;
	CUDA_CHECK(cudaGetDevice(&initial_device));
	for (int i = 0; i < sync_vec->size(); i++)
	{
		Solver<float> *solver;
		if (i == 0)
		{
			solver = sync_ptr->solver().get();
		}
		else
		{
			solver = (*sync_vec)[i]->solver().get();
		}

		Net<float> *net = solver->net().get();
		CUDA_CHECK(cudaSetDevice(solver->param().device_id()));
		net->ForwardPrefilled();
		CUDA_CHECK(cudaSetDevice(initial_device));
	}
}

/** -----------------------------------------------------------------
** Available commands.
**/
struct handler_registry {
  string cmd;
  void(*func)(MEX_ARGS);
};

static handler_registry handlers[] = {
  // Public API functions
  { "get_solver", get_solver },
  { "solver_get_attr", solver_get_attr },
  { "solver_get_iter", solver_get_iter },
  { "solver_restore", solver_restore },
  { "solver_solve", solver_solve },
  { "solver_step", solver_step },
  { "solver_snapshot", solver_snapshot },
  { "solver_teststep_multigpu", solver_teststep_multigpu },
  { "get_solver_multigpu", get_solver_multigpu },
  { "get_net", get_net },
  { "net_get_attr", net_get_attr },
  { "net_forward", net_forward },
  { "net_backward", net_backward },
  { "net_copy_from", net_copy_from },
  { "net_reshape", net_reshape },
  { "net_save", net_save },
  { "layer_get_attr", layer_get_attr },
  { "layer_get_type", layer_get_type },
  { "blob_get_shape", blob_get_shape },
  { "blob_reshape", blob_reshape },
  { "blob_get_data", blob_get_data },
  { "blob_get_data_byname_multigpu", blob_get_data_byname_multigpu },	// non-OO implementation
  { "blob_set_data", blob_set_data },
  { "blob_set_data_multigpu", blob_set_data_multigpu },
  { "blob_get_diff", blob_get_diff },
  { "blob_set_diff", blob_set_diff },
  { "set_mode_cpu", set_mode_cpu },
  { "set_mode_gpu", set_mode_gpu },
  { "set_device", set_device },
  { "get_init_key", get_init_key },
  { "reset", reset },
  { "read_mean", read_mean },
  { "write_mean", write_mean },
  { "init_log", init_log },
  // The end.
  { "END", NULL },
};

/** -----------------------------------------------------------------
** matlab entry point.
**/
// Usage: caffe_(api_command, arg1, arg2, ...)
void mexFunction(MEX_ARGS) {
  if (init_key == -2) {
    init_key = static_cast<double>(caffe_rng_rand());
    initGlog();
  }
  mexLock();  // Avoid clearing the mex file.
  mxCHECK(nrhs > 0, "Usage: caffe_(api_command, arg1, arg2, ...)");
  {// Handle input command
    char* cmd = mxArrayToString(prhs[0]);
    bool dispatched = false;
    // Dispatch to cmd handler
    for (int i = 0; handlers[i].func != NULL; i++) {
      if (handlers[i].cmd.compare(cmd) == 0) {
        handlers[i].func(nlhs, plhs, nrhs - 1, prhs + 1);
        dispatched = true;
        break;
      }
    }
    if (!dispatched) {
      ostringstream error_msg;
      error_msg << "Unknown command '" << cmd << "'";
      mxERROR(error_msg.str().c_str());
    }
    mxFree(cmd);
  }
}
