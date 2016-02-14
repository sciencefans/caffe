function varargout = passthrough(varargin)
    varargout = cell(1,nargout);
    if nargout == 0
        caffe_(varargin{:});
    else
        [varargout{:}] = caffe_(varargin{:});
    end
end
