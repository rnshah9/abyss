#include "RAlgorithmsShort.h"
#include "Contigs.h"
#include "RUtils.h"
#include "SequenceTree.h"

#include "btllib/include/btllib/seq_reader.hpp"

#if _OPENMP
#include <omp.h>
#endif

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <set>

template<std::size_t N, class T>
constexpr std::size_t countof(T(&)[N]) { return N; }

typedef std::map<unsigned int, std::map<unsigned int, Support>> SupportMap;
typedef std::map<unsigned int, SupportMap> RepeatSupportMap;

long ReadSize::readsSampleSize = 0;
std::vector<ReadSize> ReadSize::readSizes;
ReadSize ReadSize::current(0);

class FractionHistogram : public Histogram
{

  public:
	void insert(double fraction)
	{
		assert(fraction >= 0);
		assert(fraction <= 1);
		Histogram::insert(int(fraction * 100));
	}

	friend std::ostream& operator<<(std::ostream& o, const FractionHistogram& h)
	{
		o << (Histogram&)h;
		if (h.size() == 0 || (--h.end())->first != 100) {
			o << 100 << "\t0\n";
		}
		return o;
	}
};

class Resolution
{

  public:
	Resolution(const ReadSize& batch, int r)
	  : batch(batch)
	  , r(r)
	{}

	RepeatSupportMap repeatSupportMap;
	const ReadSize& batch;
	int r;
	Histogram findsHistogram;
	FractionHistogram fractionFindsHistogram;
	Histogram calculatedTestsHistogram;
	bool failed = false;
};

static int
getMinWindowLength(const int tests, const int repeatSize, const int minMargin)
{
	return tests - 1 + minMargin + repeatSize + minMargin;
}

static bool
windowLongEnough(const int windowSize, const int tests, const int repeatSize, const int minMargin)
{
	return windowSize >= getMinWindowLength(tests, repeatSize, minMargin);
}

// static int numOfTests(const int repeatSize, const int windowSize, const int minMargin) {
//  return windowSize - minMargin - repeatSize - minMargin + 1;
//}

static int
getMargin(const int windowSize, const int tests, const int repeatSize, const int minMargin)
{
	(void)minMargin;
	assert(windowLongEnough(windowSize, tests, repeatSize, minMargin));
	const int requiredSeqSize = windowSize + tests - 1;
	const int margin = (requiredSeqSize - repeatSize + 1) / 2;
	assert(margin >= minMargin);
	return margin;
}

static bool
determineShortReadStats(const std::vector<std::string>& readFilenames)
{
	if (opt::verbose) {
		std::cerr << "Determining read stats..." << std::endl;
	}
	ReadSize::readSizes.clear();
#pragma omp parallel
#pragma omp single
	{
#if _OPENMP
		for (const auto& f : readFilenames) {
			const auto filename =
			    f; // Clang complains that range based loop is copying without this
#else
		for (const auto& filename : readFilenames) {
#endif
#pragma omp task firstprivate(filename)
			{
				Histogram hist;
				//std::map<int, Histogram> qualThresholdPositionsHists;

				btllib::SeqReader reader(filename, btllib::SeqReader::Flag::SHORT_MODE);
				for (btllib::SeqReader::Record record;
				     (record = reader.read()) && (record.num < READ_STATS_SAMPLE_SIZE);) {
					if (record.seq.size() > opt::maxReadSize) {
						continue;
					}
					hist.insert(record.seq.size());
					/*for (int j = record.qual.size() - 1; j >= 0; j--) {
						if (record.qual[j] >= opt::readQualityThreshold) {
							qualThresholdPositionsHists[record.seq.size()].insert(j);
							break;
						}
					}*/
				}

#pragma omp critical(ReadSizes)
				{
					for (const auto& i : hist) {
						ReadSize* batch = nullptr;
						bool found = false;
						for (auto& b : ReadSize::readSizes) {
							if (b.size == i.first) {
								found = true;
								batch = &b;
								break;
							}
						}
						if (!found) {
							ReadSize::readSizes.push_back(ReadSize(i.first));
							batch = &(ReadSize::readSizes.back());
						}
						batch->sampleCount += i.second;
						/*auto& qualHist = batch->qualThresholdPositions;
						for (const auto& q : qualThresholdPositionsHists[i.first]) {
							for (size_t n = 0; n < q.second; n++) {
								qualHist.insert(q.first);
							}
						}*/
					}
				}
			}
		}
#pragma omp taskwait
	}

	ReadSize::readsSampleSize = 0;
	for (const auto& batch : ReadSize::readSizes) {
		ReadSize::readsSampleSize += batch.sampleCount;
	}

	if (ReadSize::readSizes.size() == 0) {
		std::cerr << "Insufficient number of short reads. Finishing..." << std::endl;
		return false;
	}

	std::sort(ReadSize::readSizes.begin(), ReadSize::readSizes.end(), [](ReadSize a, ReadSize b) {
		return a.size < b.size;
	});

	decltype(ReadSize::readSizes) mergedReadSizes;
	std::set<size_t> idxToSkip;
	for (size_t i = 0; i < ReadSize::readSizes.size() - 1; i++) {
		if (idxToSkip.find(i) != idxToSkip.end()) {
			continue;
		}
		int mergeCount = 0;
		ReadSize::readSizes[i].sizeAndMergedSizes.insert(ReadSize::readSizes[i].size);
		for (size_t j = i + 1; j < ReadSize::readSizes.size(); j++) {
			if (ReadSize::readSizes[j].size - ReadSize::readSizes[i].size <= 2) {
				/*for (const auto& q : ReadSize::readSizes[j].qualThresholdPositions) {
					for (size_t n = 0; n < q.second; n++) {
						ReadSize::readSizes[j].qualThresholdPositions.insert(q.first);
					}
				}*/
				ReadSize::readSizes[i].sizeAndMergedSizes.insert(ReadSize::readSizes[j].size);
				if (ReadSize::readSizes[i].sampleCount <= ReadSize::readSizes[j].sampleCount) {
					ReadSize::readSizes[i].size = ReadSize::readSizes[j].size;
				}
				ReadSize::readSizes[i].sampleCount += ReadSize::readSizes[j].sampleCount;
				idxToSkip.insert(j);
				mergeCount++;
				if (mergeCount >= 3) {
					break;
				}
			}
		}
		mergedReadSizes.push_back(ReadSize::readSizes[i]);
	}
	if (idxToSkip.find(ReadSize::readSizes.size() - 1) == idxToSkip.end()) {
		ReadSize::readSizes.back().sizeAndMergedSizes.insert(ReadSize::readSizes.back().size);
		mergedReadSizes.push_back(ReadSize::readSizes.back());
	}
	ReadSize::readSizes = mergedReadSizes;

	std::sort(ReadSize::readSizes.begin(), ReadSize::readSizes.end(), [](ReadSize a, ReadSize b) {
		return a.sampleCount > b.sampleCount;
	});

	if (ReadSize::readSizes[0].getFractionOfTotal() < READ_BATCH_FRACTION_THRESHOLD) {
		std::cerr << "Insufficient reads of same size. Finishing..." << std::endl;
		return false;
	}

	std::vector<ReadSize> readSizesFiltered;
	for (const auto& b : ReadSize::readSizes) {
		if (b.getFractionOfTotal() >= READ_BATCH_FRACTION_THRESHOLD) {
			readSizesFiltered.push_back(b);
		}
	}
	ReadSize::readSizes = readSizesFiltered;

	std::sort(ReadSize::readSizes.begin(), ReadSize::readSizes.end(), [](ReadSize a, ReadSize b) {
		return a.size < b.size;
	});
	if (opt::verbose) {
		std::cerr << "Read lengths determined to be: " << std::fixed;
		std::cerr << ReadSize::readSizes[0].size << " ("
		          << (ReadSize::readSizes[0].getFractionOfTotal() * 100.0) << "%)";
		for (size_t i = 1; i < ReadSize::readSizes.size(); i++) {
			std::cerr << ", " << ReadSize::readSizes[i].size << " ("
			          << (ReadSize::readSizes[i].getFractionOfTotal() * 100.0) << "%)";
		}
		std::cerr << std::defaultfloat << std::endl;
	}

	if ((opt::rValues.size() > 0) && (opt::rValues.size() < ReadSize::readSizes.size())) {
		std::cerr << opt::rValues.size() << " r values provided, " << ReadSize::readSizes.size() << " needed." << std::endl;;
		std::exit(-1);
	}

	std::sort(opt::rValues.begin(), opt::rValues.end());
	for (size_t i = 0; i < ReadSize::readSizes.size(); i++) {
		auto& batch = ReadSize::readSizes[i];
		if (opt::rValues.size() > 0) {
			if (int(i) >= int(opt::rValues.size())) { continue; }
			const int r = opt::rValues[i - (ReadSize::readSizes.size() - opt::rValues.size())];
			if (r <= int(opt::k)) {
				std::cerr << "r size (" << r << ") must be larger than assembly k (" << opt::k << ")." << std::endl;
				std::exit(-1);
			}
			if (r > batch.size - opt::extract + 1) {
				std::cerr << "r size (" << r << ") must be smaller than or equal to read size - extract + 1 (" << batch.size - opt::extract + 1 << ")." << std::endl;
				std::exit(-1);
			}
			batch.rValues.push_back(r);
		} else {
			const int r = std::min({ int(opt::k + R_HEURISTIC), int(batch.size * R_HEURISTIC_A + R_HEURISTIC_B), int(batch.size - opt::extract + 1) });
			if (r > int(opt::k)) {
				batch.rValues.push_back(r);
			}
		}
	}

	if (opt::verbose) {
		std::cerr << "Using r values: ";
		for (size_t i = 0, j = 0; i < ReadSize::readSizes.size(); i++) {
			j = 0;
			for (auto r : ReadSize::readSizes[i].rValues) {
				std::cerr << r << " (" << ReadSize::readSizes[i].size << + ")";
				if ((i < ReadSize::readSizes.size() - 1) ||
				    (j < ReadSize::readSizes[i].rValues.size() - 1)) {
					std::cerr << ", ";
				}
				j++;
			}
		}
		std::cerr << '\n';
	}

	std::sort(opt::covApproxFactors.begin(), opt::covApproxFactors.end());
	for (size_t i = 0; i < ReadSize::readSizes.size(); i++) {
		auto& batch = ReadSize::readSizes[i];
		if (int(i) < int(opt::covApproxFactors.size())) {
			batch.covApproxFactor = opt::covApproxFactors[i];
		}
	}
	if (opt::verbose) {
		std::cerr << "Using coverage approximation factors: ";
		for (size_t i = 0; i < ReadSize::readSizes.size(); i++) {
			std::cerr << ReadSize::readSizes[i].covApproxFactor << " (" << ReadSize::readSizes[i].size << + ")";
			if (i < ReadSize::readSizes.size() - 1) {
				std::cerr << ", ";
			}
		}
		std::cerr << '\n';
	}

	return true;
}

static Support
testSequence(const Sequence& sequence)
{
	static unsigned char BASES[] = { 'A', 'C', 'T', 'G' };
	int found = 0;
	int tests = 0;
	unsigned r = g_vanillaBloom->get_k();
	if (sequence.size() >= r) {
		tests = sequence.size() - r + 1;
		int offset = 0;
		if (opt::errorCorrection) {
			btllib::NtHash nthash(sequence, g_vanillaBloom->get_hash_num(), r);
			for (const auto& hitSeeds : g_spacedSeedsBloom->contains(sequence)) {
				nthash.roll();
				if (hitSeeds.size() > 0) {
					nthash.sub({}, {});
					if (g_vanillaBloom->contains(nthash.hashes())) {
						found++;
					} else {
						bool success = false;
						for (const auto& hitSeed : hitSeeds) {
							const auto seed = g_spacedSeedsBloom->get_parsed_seeds()[hitSeed];
							for (auto seedIt =
							         (seed.begin() +
							          std::round(
							              seed.size() * (1.00 - SPACED_SEEDS_SNP_FRACTION)));
							     seedIt != seed.end();
							     ++seedIt) {
								const auto pos = *seedIt;
								for (auto base : BASES) {
									if (base == (unsigned char)(sequence[offset + pos])) {
										continue;
									}
									nthash.sub({ pos }, { base });
									if (g_vanillaBloom->contains(nthash.hashes())) {
										success = true;
										found++;
										break;
									}
								}
								if (success) {
									break;
								}
							}
							if (success) {
								break;
							}
						}
					}
				}
				offset++;
			}
		} else {
			found = g_vanillaBloom->contains(sequence);
		}
	}
	return Support(found, tests);
}

static Support
testCombination(
    const std::string& head,
    const std::string& repeat,
    const std::string& tail,
    const int requestedTests)
{
	const auto windowSize = g_vanillaBloom->get_k();

	auto plannedTests = requestedTests;
	if (plannedTests < opt::minTests) {
		plannedTests = opt::minTests;
	}

	int possibleTests = head.size() + repeat.size() + tail.size() - windowSize + 1;
	if (possibleTests < plannedTests) {
		return Support(Support::UnknownReason::POSSIBLE_TESTS_LT_PLANNED);
	}

	if (plannedTests > opt::maxTests) {
		return Support(Support::UnknownReason::OVER_MAX_TESTS);
	}

	const auto margin = getMargin(windowSize, plannedTests, repeat.size(), MIN_MARGIN);

	if (long(head.size()) < margin) {
		return Support(Support::UnknownReason::HEAD_SHORTER_THAN_MARGIN);
	}

	if (long(tail.size()) < margin) {
		return Support(Support::UnknownReason::TAIL_SHORTER_THAN_MARGIN);
	}

	Sequence sequence;
	if (possibleTests > plannedTests + 1) {
		assert(long(head.size()) > margin || long(tail.size()) > margin);
		sequence = head.substr(head.size() - margin) + repeat + tail.substr(0, margin);
	} else {
		sequence = head + repeat + tail;
	}
	possibleTests = sequence.size() - windowSize + 1;

	assert(plannedTests <= possibleTests);
	assert(possibleTests <= plannedTests + 1);
	assert(int(sequence.size()) >= MIN_MARGIN + int(repeat.size()) + MIN_MARGIN);
	assert(int(sequence.size()) < int(windowSize) * 2);

	return testSequence(sequence);
}

static double
expectedSpacingBetweenReads(const ContigPath& path)
{
	assert(path.size() >= 3);
	const long pathLength = 1000000; // Use long path lenth in order to calculate numbers asymptotically
	std::vector<double> contigBaseCoverages;
	for (const auto& node : path) {
		contigBaseCoverages.push_back(getContigBaseCoverage(node));
	}
	const double pathBaseCoverage = *std::min_element(contigBaseCoverages.begin(), contigBaseCoverages.end());
	const double pathBases = pathBaseCoverage * pathLength;

	double meanReadKmerContribution = 0;
	for (const auto& batch : ReadSize::readSizes) {
		meanReadKmerContribution += batch.getFractionOfTotal() * (batch.size - opt::k + 1);
	}
	const double baseContributionRatio = ReadSize::current.getFractionOfTotal() *
	                                     (ReadSize::current.size - opt::k + 1) /
	                                     meanReadKmerContribution;

	const double approxNumOfReads = double(pathBases * baseContributionRatio) /
	                                double(opt::k * (ReadSize::current.size - opt::k + 1));
	assert(approxNumOfReads > 2);

	const double expectedSpacing = std::max(double(1.0), double(pathLength - ReadSize::current.size + 1) / double(approxNumOfReads));

	return expectedSpacing;
}

static Support
determinePathSupport(const ContigPath& path)
{
	assert(path.size() >= 3);
	const Sequence repeat = getPathSequence(ContigPath(path.begin() + 1, path.end() - 1));
	const int repeatSize = repeat.size();
	assert(repeatSize >= 2);

	const long calculatedTests =
	    std::round(expectedSpacingBetweenReads(path) * ReadSize::current.covApproxFactor + opt::threshold);
	assert(calculatedTests >= 0);

	long requiredTests = calculatedTests;
	if (requiredTests < opt::minTests) {
		requiredTests = opt::minTests;
	}
	if (requiredTests > opt::maxTests) {
		return Support(calculatedTests, Support::UnknownReason::OVER_MAX_TESTS);
	}

	const int windowSize = g_vanillaBloom->get_k();
	assert(windowSize >= 4);

	if (!windowLongEnough(windowSize, requiredTests, repeatSize, MIN_MARGIN)) {
		return Support(calculatedTests, Support::UnknownReason::WINDOW_NOT_LONG_ENOUGH);
	}

	const auto& leftContig = path[0];
	const auto& rightContig = path[path.size() - 1];
	assert(windowSize >= MIN_MARGIN + repeatSize + MIN_MARGIN);

	const int leftDistance = distanceBetween(leftContig, path[1]);
	const int rightDistance = distanceBetween(path[path.size() - 2], rightContig);

	const int margin = getMargin(windowSize, requiredTests, repeat.size(), MIN_MARGIN);

	auto heads = getTreeSequences(leftContig, -leftDistance, margin, false, 2 * opt::branching);
	auto tails = getTreeSequences(rightContig, -rightDistance, margin, true, 2 * opt::branching);
	long combinations = heads.size() * tails.size();
	assert(combinations > 0);
	if (combinations > opt::branching * opt::branching) {
		std::random_shuffle(heads.begin(), heads.end());
		std::random_shuffle(tails.begin(), tails.end());
		if (heads.size() > size_t(opt::branching) && tails.size() > size_t(opt::branching)) {
			heads.resize(opt::branching);
			tails.resize(opt::branching);
		} else if (tails.size() <= size_t(opt::branching)) {
			size_t newsize = size_t(opt::branching * opt::branching) / tails.size();
			if (newsize < heads.size()) { heads.resize(newsize); }
		} else if (heads.size() <= size_t(opt::branching)) {
			size_t newsize = size_t(opt::branching * opt::branching) / heads.size();
			if (newsize < tails.size()) { tails.resize(newsize); }
		} else {
			assert(false);
		}
		combinations = heads.size() * tails.size();
		assert(combinations > 0);
	}

	for (const auto& head : heads) {
		if (long(head.size()) < margin) {
			return Support(calculatedTests, Support::UnknownReason::HEAD_SHORTER_THAN_MARGIN);
		}
	}
	for (const auto& tail : tails) {
		if (long(tail.size()) < margin) {
			return Support(calculatedTests, Support::UnknownReason::TAIL_SHORTER_THAN_MARGIN);
		}
	}

	Support maxSupport(calculatedTests, Support::UnknownReason::UNDETERMINED);
	bool unknown = false;

	if (combinations >= PATH_COMBINATIONS_MULTITHREAD_THRESHOLD) {
		bool end = false;
#if _OPENMP
		for (const auto& h : heads) {
			const auto head = h;
#else
		for (const auto& head : heads) {
#endif
#pragma omp critical(maxSupport)
			{
				if (unknown) {
					end = true;
				}
			}
			if (end) {
				break;
			}

#pragma omp task firstprivate(head) shared(maxSupport, unknown)
			{
				bool end = false;
				for (const auto& tail : tails) {
#pragma omp critical(maxSupport)
					{
						if (unknown) {
							end = true;
						}
					}
					if (end) {
						break;
					}

					auto support = testCombination(head, repeat, tail, requiredTests);

#pragma omp critical(maxSupport)
					{
						if (support.unknown()) {
							unknown = true;
							end = true;
							maxSupport = support;
						} else if (support > maxSupport) {
							maxSupport = support;
						} else if (maxSupport.found == 0 && support.tests > maxSupport.tests) {
							maxSupport.tests = support.tests;
						}
					}
					if (end) {
						break;
					}
				}
			}
		}
	} else {
		for (const auto& head : heads) {
			if (unknown) {
				break;
			}

			for (const auto& tail : tails) {
				if (unknown) {
					break;
				}

				auto support = testCombination(head, repeat, tail, requiredTests);

				if (support.unknown()) {
					unknown = true;
					maxSupport = support;
					break;
				} else if (support > maxSupport) {
					maxSupport = support;
				} else if (maxSupport.found == 0 && support.tests > maxSupport.tests) {
					maxSupport.tests = support.tests;
				}
			}
		}
	}

	if (combinations >= PATH_COMBINATIONS_MULTITHREAD_THRESHOLD) {
#pragma omp taskwait
	}

	maxSupport.calculatedTests = calculatedTests;
	return maxSupport;
}

static SupportMap
buildRepeatSupportMap(const ContigNode& repeat)
{
	SupportMap supportMap;
	bool unknown = false;
	in_edge_iterator inIt, inLast;
	for (std::tie(inIt, inLast) = in_edges(repeat, g_contigGraph); inIt != inLast; ++inIt) {
		const auto intig = source(*inIt, g_contigGraph);
		out_edge_iterator outIt, outLast;
		for (std::tie(outIt, outLast) = out_edges(repeat, g_contigGraph); outIt != outLast;
		     ++outIt) {
			const auto outig = target(*outIt, g_contigGraph);
			auto support = determinePathSupport({ intig, repeat, outig });

			supportMap[intig.index()][outig.index()] = support;
			if (support.unknown()) {
				unknown = true;
			}
		}
	}

	if (unknown) {
		for (std::tie(inIt, inLast) = in_edges(repeat, g_contigGraph); inIt != inLast; ++inIt) {
			const auto intig = source(*inIt, g_contigGraph);
			out_edge_iterator outIt, outLast;
			for (std::tie(outIt, outLast) = out_edges(repeat, g_contigGraph); outIt != outLast;
			     ++outIt) {
				const auto outig = target(*outIt, g_contigGraph);
				auto& support = supportMap[intig.index()][outig.index()];
				if (!support.unknown()) {
					support.reset();
					support.unknownReason = Support::UnknownReason::DIFFERENT_CULPRIT;
				}
			}
		}
	}

	return supportMap;
}

static void
updateStats(
    Resolution& resolution,
    std::vector<Support>& supports,
    const SupportMap& repeatSupportMap,
    bool inHistSample)
{
	for (const auto& intigIdxAndOutigsSupp : repeatSupportMap) {
		for (const auto& outigIdxAndSupp : intigIdxAndOutigsSupp.second) {
			const auto& support = outigIdxAndSupp.second;

			supports.push_back(support);

			if (!support.unknown()) {
				assert(support.found >= 0);
				assert(support.tests >= 0);
				if (inHistSample) {
					resolution.findsHistogram.insert(support.found);
					resolution.fractionFindsHistogram.insert(
					    double(support.found) / double(support.tests));
				}
			}

			assert(support.calculatedTests >= 0);
			if (inHistSample) {
				resolution.calculatedTestsHistogram.insert(support.calculatedTests);
			}
		}
	}
}

static bool
isSmallRepeat(const ContigNode& node)
{
	unsigned r = g_vanillaBloom->get_k();
	return (
	    !get(vertex_removed, g_contigGraph, node) && !node.sense() &&
	    windowLongEnough(r, opt::minTests, getContigSize(node), MIN_MARGIN) &&
	    (in_degree(node, g_contigGraph) > 0 && out_degree(node, g_contigGraph) > 0) &&
	    (in_degree(node, g_contigGraph) > 1 || out_degree(node, g_contigGraph) > 1));
}

static Resolution
resolveRepeats()
{
	long total = (num_vertices(g_contigGraph) - num_vertices_removed(g_contigGraph)) / 2;
	long repeats = 0;
	long pathsSupported = 0, pathsUnsupported = 0;
	std::vector<Support> supports;

	progressStart(
	    "Path resolution (r = " + std::to_string(g_vanillaBloom->get_k()) + ")", total * 2);

	Resolution resolution(ReadSize::current, g_vanillaBloom->get_k());

	Graph::vertex_iterator vertexStart, vertexEnd;
	boost::tie(vertexStart, vertexEnd) = vertices(g_contigGraph);

	iteratorMultithreading(
	    vertexStart,
	    vertexEnd,
	    [&](const ContigNode& node) {
		    if (!get(vertex_removed, g_contigGraph, node)) {
			    if (isSmallRepeat(node)) {
				    return true;
			    } else {
#pragma omp critical(cerr)
				    progressUpdate();
				    return false;
			    }
		    }
		    return false;
	    },
	    [&](const ContigNode& node) {
		    bool inHistSample;
		    bool skip = false;
#pragma omp critical(resolution)
		    {
			    repeats++;
			    inHistSample = (repeats <= HIST_SAMPLE_SIZE);
			    skip = (repeats > REPEAT_CASES_LIMIT);
		    }

		    if (!skip) {
			    auto supportMap = buildRepeatSupportMap(node);

#pragma omp critical(resolution)
			    {
				    resolution.repeatSupportMap[node.index()] = supportMap;
				    updateStats(resolution, supports, supportMap, inHistSample);
			    }
		    }

#pragma omp critical(cerr)
		    progressUpdate();
	    });

	long pathsKnown = 0, pathsUnknown = 0, pathsTotal = 0;
	static const std::string unknownReasonLabels[] = { "Undetermined", "Too many combinations", "Over max tests", "Possible tests < planned tests", "Window not long enough", "Head shorter than margin", "Tail shorter than margin", "Different culprit" };
	static const size_t unknownReasons = countof(unknownReasonLabels);
	int unknownReasonCounts[unknownReasons];
	for (size_t i = 0; i < unknownReasons; i++) {
		unknownReasonCounts[i] = 0;
	}
	for (const auto& s : supports) {
		if (s.unknown()) {
			pathsUnknown++;
			unknownReasonCounts[unsigned(s.unknownReason)]++;
		} else {
			pathsKnown++;
		}
	}
	pathsTotal = pathsKnown + pathsUnknown;

	const auto percentOrZero = [](const long num, const long denom) {
		if (denom == 0) {
			return 0.0;
		} else {
			return 100.0 * double(num) / double(denom);
		}
	};

	if (repeats > 0 && pathsKnown > 0) {
		for (const auto& findsAndCount : resolution.findsHistogram) {
			const auto& finds = findsAndCount.first;
			const auto& count = findsAndCount.second;
			if (finds >= opt::threshold) {
				pathsSupported += count;
			} else {
				pathsUnsupported += count;
			}
		}

		double sampleFactor = double(pathsKnown) / double(pathsSupported + pathsUnsupported);
		pathsSupported *= sampleFactor;
		pathsUnsupported *= sampleFactor;

		if (opt::verbose) {
			std::cerr << std::fixed;
			std::cerr << "Small repeats = " << repeats << "/" << total << " ("
			          << percentOrZero(repeats, total) << "%)\n";
			std::cerr << "Known support paths = " << pathsKnown << " / "
			          << pathsTotal << " ("
			          << percentOrZero(pathsKnown, pathsTotal) << "%)\n";
			std::cerr << "Unknown support paths = " << pathsUnknown << " / "
			          << pathsTotal << " ("
			          << percentOrZero(pathsUnknown, pathsTotal) << "%)\n";
			for (size_t i = 0; i < unknownReasons; i++) {
				if (i > 0) { std::cerr << ", "; }
				std::cerr << unknownReasonLabels[i] << ": " << percentOrZero(unknownReasonCounts[i], pathsUnknown) << "%";
			}
			std::cerr << "\n";
			std::cerr << "Supported paths ~= " << pathsSupported << "/" << pathsKnown << " ("
			          << percentOrZero(pathsSupported, pathsKnown) << "%)\n";
			std::cerr << "Unsupported paths ~= " << pathsUnsupported << "/" << pathsKnown << " ("
			          << percentOrZero(pathsUnsupported, pathsKnown) << "%)\n";
			std::cerr << std::defaultfloat << std::flush;
		}

		if (double(pathsSupported) / double(pathsKnown) < SUPPORTED_PATHS_MIN) {
			std::cerr << "Insufficient support found. Is something wrong with the data?\n";
			resolution.failed = true;
		}
	} else {
		std::cerr << "No small resolveable junctions were found!" << std::endl;

		if (opt::verbose) {
			std::cerr << std::fixed;
			std::cerr << "Small repeats = " << repeats << "/" << total << " ("
			          << percentOrZero(repeats, total) << "%)\n";
			std::cerr << "Known support paths = " << pathsKnown << " / "
			          << pathsTotal << " ("
			          << percentOrZero(pathsKnown, pathsTotal) << "%)\n";
			std::cerr << "Unknown support paths = " << pathsUnknown << " / "
			          << pathsTotal << " ("
			          << percentOrZero(pathsUnknown, pathsTotal) << "%)\n";
			for (size_t i = 0; i < unknownReasons; i++) {
				if (i > 0) { std::cerr << ", "; }
				std::cerr << unknownReasonLabels[i] << ": " << percentOrZero(unknownReasonCounts[i], pathsUnknown) << "%";
			}
			std::cerr << "\n" << std::defaultfloat << std::flush;
		}

		resolution.failed = true;
	}

	return resolution;
}

struct OldEdge
{

	OldEdge(ContigNode u, ContigNode v)
	  : u(u)
	  , v(v)
	{}

	ContigNode u, v;
};

struct NewEdge
{

	NewEdge(ContigNode u, ContigNode v, Distance distance)
	  : u(u)
	  , v(v)
	  , distance(distance)
	{}

	ContigNode u, v;
	Distance distance;
};

struct NewVertex
{

	NewVertex(ContigNode original, ContigNode node)
	  : original(original)
	  , node(node)
	{}

	ContigNode original, node;
};

class RepeatInstance
{

  public:
	RepeatInstance(
	    const ContigNode instance,
	    const ContigNode original,
	    const std::vector<ContigNode> originalIntigs,
	    const std::vector<ContigNode> originalOutigs)
	  : instance(instance)
	  , original(original)
	  , originalIntigs(originalIntigs)
	  , originalOutigs(originalOutigs)
	{}

	bool inOriginalIntigs(const ContigNode& node) const
	{
		return std::find(originalIntigs.begin(), originalIntigs.end(), node) !=
		       originalIntigs.end();
	}

	bool inOriginalOutigs(const ContigNode& node) const
	{
		return std::find(originalOutigs.begin(), originalOutigs.end(), node) !=
		       originalOutigs.end();
	}

	RepeatInstance getReverse() const
	{
		std::vector<ContigNode> originalIntigsReverse;
		for (auto originalOutig : originalOutigs) {
			originalIntigsReverse.push_back(originalOutig ^ true);
		}

		std::vector<ContigNode> originalOutigsReverse;
		for (auto originalIntig : originalIntigs) {
			originalOutigsReverse.push_back(originalIntig ^ true);
		}

		return RepeatInstance(
		    instance ^ true, original ^ true, originalIntigsReverse, originalOutigsReverse);
	}

	const ContigNode instance;
	const ContigNode original;
	std::vector<ContigNode> originalIntigs;
	std::vector<ContigNode> originalOutigs;
	std::vector<std::reference_wrapper<const RepeatInstance>> intigsInstances;
	std::vector<std::reference_wrapper<const RepeatInstance>> outigsInstances;
};

static void
processGraph(
    const Resolution& resolution,
    ImaginaryContigPaths& supportedPaths,
    ImaginaryContigPaths& unsupportedPaths)
{
	progressStart("New paths and vertices setup", resolution.repeatSupportMap.size() * 3);

	assert(!resolution.failed);

	std::vector<OldEdge> edges2remove;
	std::vector<NewEdge> edges2add;
	std::vector<NewVertex> vertices2add;

	std::map<int, std::vector<RepeatInstance>> repeatInstancesMap;

	size_t lastId = num_vertices(g_contigGraph) / 2;

	const auto start = resolution.repeatSupportMap.begin();
	const auto end = resolution.repeatSupportMap.end();

#ifdef _OPENMP
	int threads = omp_get_num_threads();
#else
	int threads = 1;
#endif

	// 1
	iteratorMultithreading(
	    start,
	    end,
	    [&](const std::pair<int, SupportMap>& repeatSupport) {
		    (void)repeatSupport;
		    return true;
	    },
	    [&](const std::pair<int, SupportMap>& repeatSupport) {
		    const auto repeat = ContigNode(repeatSupport.first);
		    const auto& supportMap = repeatSupport.second;

#pragma omp critical(repeatInstancesMap)
		    {
			    repeatInstancesMap.emplace(repeat.index(), std::vector<RepeatInstance>());
			    repeatInstancesMap.emplace((repeat ^ true).index(), std::vector<RepeatInstance>());
		    }

		    for (const auto& intigIdxAndOutigsSupp : supportMap) {
			    const auto intig = ContigNode(intigIdxAndOutigsSupp.first);
			    for (const auto& outigIdxAndSupp : intigIdxAndOutigsSupp.second) {
				    const auto outig = ContigNode(outigIdxAndSupp.first);
				    const auto& support = outigIdxAndSupp.second;
				    int dist1 = distanceBetween(intig, repeat);
				    int dist2 = distanceBetween(repeat, outig);

				    ImaginaryContigPath path = { { intig, 0 },
					                             { repeat, dist1 },
					                             { outig, dist2 } };

				    if (support.good()) {
#pragma omp critical(supportedPaths)
					    supportedPaths.insert(path);
				    } else {
#pragma omp critical(unsupportedPaths)
					    unsupportedPaths.insert(path);

#pragma omp critical(supportedPaths)
					    {
						    if (supportedPaths.find(path) != supportedPaths.end()) {
							    supportedPaths.erase(path);
						    }
					    }
				    }
			    }
		    }

#pragma omp critical(cerr)
		    progressUpdate();
	    },
	    std::min(4, threads));

	// 2
	for (auto it = start; it != end; it++) {
		const std::pair<int, SupportMap>& repeatSupport = *it;
		const auto repeat = ContigNode(repeatSupport.first);
		const auto& supportMap = repeatSupport.second;

		assert(repeatInstancesMap.find(repeat.index()) != repeatInstancesMap.end());
		assert(repeatInstancesMap.find((repeat ^ true).index()) != repeatInstancesMap.end());

		auto& repeatInstances = repeatInstancesMap.at(repeat.index());
		auto& repeatInstancesReverse = repeatInstancesMap.at((repeat ^ true).index());

		assert(repeatInstances.size() == 0);
		assert(repeatInstancesReverse.size() == 0);

		for (const auto& intigIdxAndOutigsSupp : supportMap) {
			const auto intig = ContigNode(intigIdxAndOutigsSupp.first);
			const auto& outigsSupp = intigIdxAndOutigsSupp.second;

			std::vector<ContigNode> supportedOutigs;
			for (const auto& outigIdxAndSupp : outigsSupp) {
				const auto& outig = ContigNode(outigIdxAndSupp.first);
				const auto& support = outigIdxAndSupp.second;
				if (support.good()) {
					supportedOutigs.push_back(outig);
				}
			}

			bool matched = false;
			for (auto& instance : repeatInstances) {
				if (instance.originalOutigs.size() == supportedOutigs.size()) {
					matched = true;
					for (const auto& outig : supportedOutigs) {
						bool found = false;
						for (const auto& instanceOutig : instance.originalOutigs) {
							if (outig == instanceOutig) {
								found = true;
								break;
							}
						}
						if (!found) {
							matched = false;
							break;
						}
					}
				}
				if (matched) {
					instance.originalIntigs.push_back(intig);
					break;
				}
			}

			if (!matched) {
				if (supportedOutigs.size() > 0) {
					std::vector<ContigNode> intigs = { intig };
					if (repeatInstances.size() == 0) {
						repeatInstances.push_back(
						    RepeatInstance(repeat, repeat, intigs, supportedOutigs));
					} else {
						ContigNode repeatCopy = ContigNode(lastId++, repeat.sense());
						repeatInstances.push_back(
						    RepeatInstance(repeatCopy, repeat, intigs, supportedOutigs));
					}
				}
			}
		}

		if (repeatInstances.size() > 0) {
			std::set<int> intigIdxs;

			for (const auto& instance : repeatInstances) {
				for (const auto& intig : instance.originalIntigs) {
					assert(intigIdxs.find(intig.index()) == intigIdxs.end());
					intigIdxs.insert(intig.index());
				}
				assert(instance.originalOutigs.size() > 0);
				repeatInstancesReverse.push_back(instance.getReverse());
			}
		} else {
			auto instance = RepeatInstance(repeat, repeat, {}, {});
			repeatInstances.push_back(instance);
			repeatInstancesReverse.push_back(instance.getReverse());
		}

		progressUpdate();
	}

	// 3
	iteratorMultithreading(
	    start,
	    end,
	    [&](const std::pair<int, SupportMap>& repeatSupport) {
		    (void)repeatSupport;
		    return true;
	    },
	    [&](const std::pair<int, SupportMap>& repeatSupport) {
		    const auto repeat = ContigNode(repeatSupport.first);

		    auto& repeatInstances = repeatInstancesMap.at(repeat.index());

		    std::list<RepeatInstance> tempInstances;

		    for (auto& instance : repeatInstances) {
			    for (const auto& intig : instance.originalIntigs) {
				    if (repeatInstancesMap.find(intig.index()) != repeatInstancesMap.end()) {
					    const auto& intigInstances = repeatInstancesMap.at(intig.index());
					    for (const auto& intigInstance : intigInstances) {
						    if (intigInstance.inOriginalOutigs(repeat)) {
							    instance.intigsInstances.push_back(intigInstance);
						    }
					    }
				    } else {
					    tempInstances.push_back(RepeatInstance(intig, intig, {}, {}));
					    instance.intigsInstances.push_back(tempInstances.back());
				    }
			    }

			    for (const auto& outig : instance.originalOutigs) {
				    if (repeatInstancesMap.find(outig.index()) != repeatInstancesMap.end()) {
					    const auto& outigInstances = repeatInstancesMap.at(outig.index());
					    for (const auto& outigInstance : outigInstances) {
						    if (outigInstance.inOriginalIntigs(repeat)) {
							    instance.outigsInstances.push_back(outigInstance);
						    }
					    }
				    } else {
					    tempInstances.push_back(RepeatInstance(outig, outig, {}, {}));
					    instance.outigsInstances.push_back(tempInstances.back());
				    }
			    }

			    if (instance.instance == instance.original) {
				    in_edge_iterator inIt, inLast;
				    for (std::tie(inIt, inLast) = in_edges(instance.original, g_contigGraph);
				         inIt != inLast;
				         ++inIt) {
#pragma omp critical(edges2remove)
					    edges2remove.push_back(
					        OldEdge(source(*inIt, g_contigGraph), instance.original));
				    }

				    out_edge_iterator outIt, outLast;
				    for (std::tie(outIt, outLast) = out_edges(instance.original, g_contigGraph);
				         outIt != outLast;
				         ++outIt) {
#pragma omp critical(edges2remove)
					    edges2remove.push_back(
					        OldEdge(instance.original, target(*outIt, g_contigGraph)));
				    }
			    } else {
#pragma omp critical(vertices2add)
				    vertices2add.push_back(NewVertex(instance.original, instance.instance));
			    }

			    for (const RepeatInstance& intigInstance : instance.intigsInstances) {
#pragma omp critical(edges2add)
				    edges2add.push_back(NewEdge(
				        intigInstance.instance,
				        instance.instance,
				        get(edge_bundle,
				            g_contigGraph,
				            edge(intigInstance.original, instance.original, g_contigGraph).first)));
			    }

			    for (const RepeatInstance& outigInstance : instance.outigsInstances) {
#pragma omp critical(edges2add)
				    edges2add.push_back(NewEdge(
				        instance.instance,
				        outigInstance.instance,
				        get(edge_bundle,
				            g_contigGraph,
				            edge(instance.original, outigInstance.original, g_contigGraph).first)));
			    }
		    }

#pragma omp critical(cerr)
		    progressUpdate();
	    });

	std::sort(
	    vertices2add.begin(), vertices2add.end(), [](const NewVertex& v1, const NewVertex& v2) {
		    return v1.node.index() < v2.node.index();
	    });
	std::sort(edges2add.begin(), edges2add.end(), [](const NewEdge& e1, const NewEdge& e2) {
		return (e1.u.index() < e2.u.index()) ||
		       (e1.u.index() == e2.u.index() && e1.v.index() < e2.v.index());
	});

	int modifications = edges2remove.size() + vertices2add.size() + edges2add.size();
	progressStart("Graph modification", modifications);

	g_contigNames.unlock();
	for (const auto& oldEdge : edges2remove) {
		if (g_contigGraph.edge(oldEdge.u, oldEdge.v).second) {
			remove_edge(oldEdge.u, oldEdge.v, g_contigGraph);
		}

		progressUpdate();
	}
	for (const auto& newVertex : vertices2add) {
		assert(in_degree(newVertex.original, g_contigGraph) == 0);
		assert(out_degree(newVertex.original, g_contigGraph) == 0);

		assert(g_contigSequences.size() == newVertex.node.index());
		assert(g_contigComments.size() == newVertex.node.id());

		g_contigSequences.push_back(getContigSequence(newVertex.original));
		g_contigSequences.push_back(getContigSequence(newVertex.original ^ true));

		std::string name = createContigName();
		put(vertex_name, g_contigGraph, newVertex.node, name);
		add_vertex(get(vertex_bundle, g_contigGraph, newVertex.original), g_contigGraph);

		g_contigComments.push_back(getContigComment(newVertex.original));

		assert(in_degree(newVertex.node, g_contigGraph) == 0);
		assert(out_degree(newVertex.node, g_contigGraph) == 0);

		progressUpdate();
	}
	for (const auto& newEdge : edges2add) {
		if (!g_contigGraph.edge(newEdge.u, newEdge.v).second) {
			g_contigGraph.add_edge(newEdge.u, newEdge.v, newEdge.distance);
		}

		progressUpdate();
	}
	g_contigNames.lock();
}

void
writeHistograms(const Resolution& resolution, const std::string& prefix, int subiteration)
{
	if (opt::verbose) {
		std::cerr << "Writing algorithm histograms..." << std::flush;
	}

	std::string findsFilename = prefix + "-r" + std::to_string(resolution.r) + "-" +
	                            std::to_string(subiteration + 1) + "-finds.tsv";
	std::ofstream findsFile(findsFilename.c_str());
	findsFile << resolution.findsHistogram;

	std::string fractionFindsFilename = prefix + "-r" + std::to_string(resolution.r) + "-" +
	                                    std::to_string(subiteration + 1) + "-percent-finds.tsv";
	std::ofstream fractionFindsFile(fractionFindsFilename.c_str());
	fractionFindsFile << resolution.fractionFindsHistogram;

	std::string calculatedTestsFilename = prefix + "-r" + std::to_string(resolution.r) + "-" +
	                                      std::to_string(subiteration + 1) +
	                                      "-calculated-tests.tsv";
	std::ofstream calculatedTestsFile(calculatedTestsFilename.c_str());
	calculatedTestsFile << resolution.calculatedTestsHistogram;

	if (opt::verbose) {
		std::cerr << " Done!" << std::endl;
	}
}

void
resolveShort(
    const std::vector<std::string>& readFilepaths,
    ImaginaryContigPaths& supportedPaths,
    ImaginaryContigPaths& unsupportedPaths)
{
	if (!determineShortReadStats(readFilepaths)) {
		return;
	}

	if (opt::verbose) {
		std::cerr << "\nRunning resolution algorithm...\n";
	}

	assert(g_contigSequences.size() > 0);
	assert(g_contigSequences.size() / 2 == g_contigComments.size());
	assert(ReadSize::readSizes.size() > 0);

	std::vector<std::pair<int, Histogram>> histograms;
	for (auto batch : ReadSize::readSizes) {
		ReadSize::current = batch;

		for (int r : ReadSize::current.rValues) {
			if (int(r) < int(opt::k)) {
				std::cerr << "r value " << r << "(" << ReadSize::current.size
				          << ") is too short - skipping." << std::endl;
				continue;
			}

			if (opt::verbose) {
				std::cerr << "\nRead size = " << batch.size << ", r = " << r << " ...\n\n";
			}

			buildFilters(readFilepaths, r, opt::bfMemFactor * double(opt::bloomSize));

			for (size_t j = 0; j < MAX_SUBITERATIONS; j++) {
				if (opt::verbose) {
					std::cerr << "\nSubiteration " << j + 1 << "...\n";
				}

				int unsupportedCountPrev = unsupportedPaths.size();

				Resolution resolution = resolveRepeats();

				if (!resolution.failed) {
					processGraph(resolution, supportedPaths, unsupportedPaths);
					assembleContigs();
					if (!opt::histPrefix.empty()) {
						writeHistograms(resolution, opt::histPrefix, j);
					}
				}

				int newUnsupportedCount = unsupportedPaths.size() - unsupportedCountPrev;
				assert(newUnsupportedCount >= 0);
				if (newUnsupportedCount == 0) {
					break;
				}
			}
		}
	}

	if (opt::verbose) {
		std::cerr << "Resolution algorithm done.\n\n";
	}
}
