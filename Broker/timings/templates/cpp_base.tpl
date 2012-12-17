#THIS FILE IS AUTOGENERATED

#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>

namespace po = boost::program_options;

namespace freedm {
namespace broker {

void CTimings::SetTimings(const std::string timingsFile)
{ 
    std::ifstream ifs;

    po::options_description loggerOpts("Timing Parameters");
    po::variables_map vm;

$parameter_block

    ifs.open(loggerCfgFile.c_str());
    if (!ifs)
    {
        Logger.Error << "Unable to timing config file: "
                << timingsFile << std::endl;
        std::exit(-1);
    }
    else
    {
        // Process the config
        po::store(parse_config_file(ifs, loggerOpts), vm);
        po::notify(vm);
        Logger.Info << "timer config file " << timingsFile <<
                " successfully loaded." << std::endl;
    }
    ifs.close();

$parameter_block2
}

}
}
