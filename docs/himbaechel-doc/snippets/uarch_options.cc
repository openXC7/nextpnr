XilinxImpl::~XilinxImpl() {};

po::options_description XilinxImpl::getUArchOptions()
{
    po::options_description specific("Xilinx specific options");
    specific.add_options()("fasm", po::value<std::string>(), "fasm bitstream output file");
    specific.add_options()("xdc", po::value<std::string>(), "name of constraints file");
    return specific;
}

