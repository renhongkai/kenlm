#include "MEMT/Decoder/Config.hh"
#include "MEMT/Decoder/Hypothesis.hh"
#include "MEMT/Decoder/Implementation.hh"
#include "MEMT/Input/Config.hh"
#include "MEMT/Input/Factory.hh"
#include "MEMT/Input/Input.hh"
#include "MEMT/Output/NullBeamDumper.hh"
#include "MEMT/Output/Oracle.hh"
#include "MEMT/Output/Top.hh"

#include "lm/sa.hh"
#include "lm/sri.hh"

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>

#include <exception>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

namespace po = boost::program_options;

class ArgumentParseError : public std::exception {
	public:
		virtual ~ArgumentParseError() throw () {}

	protected:
		ArgumentParseError() {}
};

class ArgumentCountException : public ArgumentParseError {
	public:
		ArgumentCountException(const char *key, unsigned int expected, unsigned int times)
			: key_(key), expected_(expected), times_(times) {
			what_ = "Expected ";
			what_ += key_;
			what_ += " >= ";
			what_ += boost::lexical_cast<std::string>(expected);
			what_ += " times, got it ";
			what_ += boost::lexical_cast<std::string>(times);
			what_ += ".";
		}

		virtual ~ArgumentCountException() throw() {}

		virtual const char *what() const throw() {
			return what_.c_str(); 
		}
	
	private:
		std::string key_;
		unsigned int expected_;
		unsigned int times_;

		std::string what_;
};

void CheckOnce(const po::variables_map &vm, const char **key_begin, const char **key_end) {
	for (const char **key = key_begin; key != key_end; ++key) {
		if (vm.count(*key) != 1) 
			throw ArgumentCountException(*key, 1, vm.count(*key));
	}
}

template <size_t size> void CheckOnce(
		const po::variables_map &vm,
		const char *(&keys)[size]) {
	CheckOnce(vm, keys, keys + size);
}

struct QueryConfig {
	input::Config text;
	DecoderConfig decoder;
	std::string output_oracle_prefix;
	std::string output_one_best;
	std::string input_matched;
};

class QueryConfigParser {
	public:
		QueryConfigParser();

		void Parse(std::istream &stream);

		const QueryConfig &Get() const {
			return config_;
		}

	private:
		po::options_description desc_;
		std::string confidence_string_;
		QueryConfig config_;
};


QueryConfigParser::QueryConfigParser() : desc_("Query time options") {
	desc_.add_options()
		("score.lm",
		 po::value(&config_.decoder.scorer.lm),
		 "LM scoring weight")

		("score.alignment",
		 po::value(&config_.decoder.scorer.alignment),
		 "Alignment scoring weight")

		("score.ngram",
		 po::value(&config_.decoder.scorer.ngram),
		 "NGram scoring weight")

		("score.ngram_base",
		 po::value(&config_.decoder.scorer.ngram_base)->default_value(LogScore(1.0 / 3.0)),
		 "NGram score base")

		("score.overlap",
		 po::value(&config_.decoder.scorer.overlap),
		 "Overlap scoring weight")

		("score.fuzz.ratio",
		 po::value(&config_.decoder.scorer.fuzz.ratio)->default_value(0.0),
		 "Proportion of scoring weight to randomly fuzz.  Useful for seeding MERT.")

		("beam_size",
		 po::value(&config_.decoder.internal_beam_size)->default_value(500), 
		 "Size of the decoder's internal search beam")

		("length_normalize",
		 po::value(&config_.decoder.length_normalize)->default_value(true),
		 "Langth normalize before comparing sentence end scores?")

		("output.nbest",
		 po::value(&config_.decoder.end_beam_size)->default_value(1),
		 "Number of n-best hypotheses")

		// Remember to copy to input::Config.
		("horizon.radius",
		 po::value(&config_.decoder.coverage.old_horizon)->default_value(5),
		 "Horizon radius")

		("horizon.new",
		 po::value(&config_.decoder.coverage.use_new)->default_value(false),
		 "Use the new horizon implementation?")

		("horizon.threshold",
		 po::value(&config_.decoder.coverage.stay_threshold)->default_value(0.8),
		 "New horizon threshold.")

		("output.oracle_prefix",
		 po::value(&config_.output_oracle_prefix)->default_value(std::string()),
		 "Prefix for oracle output or empty for no oracle files")

		("output.one_best",
		 po::value(&config_.output_one_best),
		 "One best output file")

		("input.matched_file",
		 po::value(&config_.input_matched),
		 "Input from matcher")

		("input.confidence",
		 po::value(&confidence_string_),
		 "Confidence values.")

		("align.pick_best",
		 po::value(&config_.text.pick_best)->default_value(false),
		 "Pick the aligned word with most confidence?")

		("align.transitive",
		 po::value(&config_.text.transitive)->default_value(false),
		 "Make alignments transitive?");
}

class BadConfidence : public ArgumentParseError {
	public:
		BadConfidence(const std::string &provided) : provided_(provided) {}

		virtual const char *what() const throw () {
			return provided_.c_str();
		}

		virtual ~BadConfidence() throw () {}

	private:
		std::string provided_;
};

void ParseConfidences(const std::string &as_string, std::vector<LinearScore> &confidences) {
	confidences.clear();
	std::istringstream parser(as_string, std::istringstream::in);
	LinearScore score;
	while (parser >> score) {
		confidences.push_back(score);
	}
	if (!parser.eof()) throw BadConfidence(as_string);
}

void QueryConfigParser::Parse(std::istream &stream) {
	po::variables_map vm;
	po::store(po::parse_config_file(stream, desc_), vm);
	po::notify(vm);
	const char *mandatory_options[] = {"score.lm", "score.alignment", "score.ngram", "score.overlap", "output.one_best", "input.matched_file", "input.confidence"};
	CheckOnce(vm, mandatory_options);
	ParseConfidences(confidence_string_, config_.text.confidences);
			
	config_.text.horizon_radius = config_.decoder.coverage.old_horizon;

	std::cout << "input.matched_file = " << config_.input_matched << std::endl;
	std::cout << config_.text << std::endl;
	std::cout << config_.decoder << std::endl;
}

struct LMConfig {
	std::string type;
	std::string file;
	unsigned int order;
};

struct ServiceConfig {
	LMConfig lm;
	short int port;
};

class NoSuchLMError : public ArgumentParseError {
	public:
		NoSuchLMError(const std::string &type) : type_(type) {
			what_ = "lm.type \"";
			what_ += type;
			what_ += "\" is not sri or salm.";
		}
		
		virtual ~NoSuchLMError() throw() {}

		virtual const char *what() throw() { return what_.c_str(); }

	private:
		std::string type_;
		std::string what_;
};

void ParseService(int argc, char *argv[], ServiceConfig &config) {
	po::options_description desc("Server Options");
	desc.add_options()
		("lm.type", po::value<std::string>(&config.lm.type)->default_value("salm"), "Language model type: sri or salm")
		("lm.file", po::value<std::string>(&config.lm.file), "File for language model")
		("lm.order", po::value<unsigned int>(&config.lm.order), "Order of language model")
		("port", po::value<short int>(&config.port), "Port");
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	const char *mandatory_options[] = {"lm.file", "lm.order", "port"};
	CheckOnce(vm, mandatory_options);
	if (config.lm.type != "salm" && config.lm.type != "sri") {
		throw NoSuchLMError(config.lm.type);
	}
}

template <class LanguageModel> void RunDecoder(const LanguageModel &model, const QueryConfig &config) {
	input::Input text;
	input::InputFactory factory;
	DecoderImpl<HypothesisCollection<DetailedScorer<LanguageModel> > > decoder;
	NullBeamDumper dumper;
	output::FileOracle oracle(config.output_oracle_prefix.c_str(), true);
	std::vector<CompletedHypothesis> nbest;
	std::ifstream matched(config.input_matched.c_str(), ios::in);
	std::ofstream one_best(config.output_one_best.c_str(), ios::out);
	output::Top top(one_best, true);
	while (factory.Make(config.text, matched, model.GetVocabulary(), text)) {
		decoder.Run(config.decoder, model, text, dumper, nbest);
		top.Write(nbest, text);
		if (!config.output_oracle_prefix.empty()) oracle.Write(nbest, text);
	}
}

template <class LMOwner> void RunLoadedService(const LMOwner &lm, short int port) {
	using boost::asio::ip::tcp;

	QueryConfigParser parser;

	boost::asio::io_service io_service;
	tcp::acceptor acceptor(io_service, tcp::endpoint(tcp::v4(), port));
	std::cerr << "Accepting connections." << std::endl;
	while (1) {
		try {
			tcp::iostream stream;
			acceptor.accept(*stream.rdbuf());
			std::cerr << "Got connection " << std::endl;
			try {
				parser.Parse(stream);
				stream.clear();
				RunDecoder(lm.GetModel(), parser.Get());
				stream << "Done" << std::endl;
			}
			catch (ArgumentParseError &e) {
				std::cerr << e.what() << std::endl;
				stream.clear();
				stream << e.what() << std::endl;
			}
		}
		catch (std::exception &e) {
			std::cerr << e.what() << std::endl;
		}
	}
}

void LoadAndRunService(const ServiceConfig &config) {
	if (config.lm.type == "sri") {
		lm::sri::Owner sri(config.lm.file.c_str(), config.lm.order);
		RunLoadedService(sri, config.port);
	} else if (config.lm.type == "salm") {
		lm::sa::Owner sa(config.lm.file.c_str(), config.lm.order);
		RunLoadedService(sa, config.port);
	}
}

} // namespace

int main(int argc, char *argv[]) {
	ServiceConfig config;
	ParseService(argc, argv, config);
	LoadAndRunService(config);
}
