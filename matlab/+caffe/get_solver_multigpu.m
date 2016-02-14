function solver = get_solver_multigpu(varargin)
% solver = get_solver(solver_file)
%   Construct a Solver object from solver_file

CHECK(ischar(varargin{1}), 'solver_file must be a string');
CHECK_FILE_EXIST(varargin{1});
pSolver = caffe_('get_solver_multigpu', varargin{:});
solver = caffe.Solver(pSolver);

end
