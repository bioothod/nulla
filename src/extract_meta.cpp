#include "nulla/iso_reader.hpp"

#include <ebucket/bucket_processor.hpp>

#include <algorithm>
#include <iostream>

#include <boost/program_options.hpp>

using namespace ioremap;

int main(int argc, char *argv[])
{
	namespace bpo = boost::program_options;

	std::vector<std::string> remotes;


	bpo::options_description generic("Ebucket example options");
	generic.add_options()
		("help", "this help message")
		;

	std::string bname, key, file;
	std::string log_file, log_level, groups;
	bpo::options_description ell("Elliptics options");
	ell.add_options()
		("remote", bpo::value<std::vector<std::string>>(&remotes)->required()->composing(), "remote node: addr:port:family")
		("log-file", bpo::value<std::string>(&log_file)->default_value("/dev/stdout"), "log file")
		("log-level", bpo::value<std::string>(&log_level)->default_value("error"), "log level: error, info, notice, debug")
		("groups", bpo::value<std::string>(&groups)->required(), "groups where bucket metadata is stored: 1:2:3")
		("bucket", bpo::value<std::string>(&bname)->required(), "use this bucket to store metadata")
		("key", bpo::value<std::string>(&key)->required(), "use this key to store metadata")
		("file", bpo::value<std::string>(&file)->required(), "parse this MP4 file")
		;

	bpo::options_description cmdline_options;
	cmdline_options.add(generic).add(ell);

	bpo::variables_map vm;

	try {
		bpo::store(bpo::command_line_parser(argc, argv).options(cmdline_options).run(), vm);

		if (vm.count("help")) {
			std::cout << cmdline_options << std::endl;
			return 0;
		}

		bpo::notify(vm);
	} catch (const std::exception &e) {
		std::cerr << "Invalid options: " << e.what() << "\n" << cmdline_options << std::endl;
		return -1;
	}

	std::string meta;
	try {
		nulla::iso_reader reader(file.c_str());
		int err = reader.parse();
		if (err < 0)
			return err;

		meta = reader.pack();
		std::cout << "Reader has loaded " << meta.size() << " bytes of metadata from " << file << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "could not parse " << file << ": " << e.what() << std::endl;
		return -1;
	}

	elliptics::file_logger log(log_file.c_str(), elliptics::file_logger::parse_level(log_level));
	std::shared_ptr<elliptics::node> node(new elliptics::node(elliptics::logger(log, blackhole::log::attributes_t())));

	std::vector<elliptics::address> rem(remotes.begin(), remotes.end());
	node->add_remote(rem);

	ebucket::bucket_processor bp(node);
	if (!bp.init(elliptics::parse_groups(groups.c_str()), std::vector<std::string>({bname}))) {
		std::cerr << "Could not initialize bucket transport, exiting";
		return -1;
	}

	ebucket::bucket b;

	elliptics::error_info err = bp.find_bucket(bname, b);
	if (err) {
		std::cerr << "Could not find bucket " << bname << " : " << err.message() << std::endl;
		return err.code();
	}

	elliptics::session s = b->session();

	auto ret = s.write_data(key, meta, 0);
	ret.wait();
	if (!ret.is_valid() || ret.error()) {
		std::cerr << "Could not write data into bucket " << b->name() <<
			", size: " << meta.size() <<
			", valid: " << ret.is_valid() <<
			", error: " << ret.error().message() <<
			std::endl;
		return err.code();
	}

	std::cout << meta.size() << " bytes of metadata from " << file << " has been uploaded into bucket " << b->name() << std::endl;
	return 0;
}
