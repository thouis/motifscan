#include "fasta_scorer.h"

#include "fasta_reader.h"
#include "fimo_style_printer.h"
#include "detail/score_matrix_detail.h"

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>

//#define LIQUIDATOR_FASTA_SCORER_TIMINGS
#ifdef LIQUIDATOR_FASTA_SCORER_TIMINGS
#include <boost/timer/timer.hpp>
#endif

#include <cerrno>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sstream>

namespace liquidator
{

void process_fasta_serial(const std::vector<ScoreMatrix>& matrices,
                          const std::string& fasta_file_path,
                          const std::string& output_file_path)
{
    std::ifstream fasta_input(fasta_file_path);
    if (!fasta_input)
    {
        throw std::runtime_error("failed to open " + fasta_file_path);
    }

    std::ofstream output(output_file_path);
    FimoStylePrinter printer(output);

    FastaReader fasta_reader(fasta_input);
    std::string sequence;
    std::string sequence_name;
    while (fasta_reader.next_read(sequence, sequence_name))
    {
        for (const auto& matrix : matrices)
        {
            printer.sequence_name = &sequence_name;
            matrix.score(sequence, printer);
        }
    }
}

std::string get_file_contents(const std::string& file_path)
{
    // Seems to be fastest way to read entire file into memory, based off of
    // http://insanecoding.blogspot.com/2011/11/how-to-read-in-file-in-c.html

    std::ifstream in(file_path, std::ios::in | std::ios::binary);
    if (!in)
    {
        std::stringstream ss;
        ss << "Failed to open file " << file_path << ": error " << errno;
        throw std::runtime_error(ss.str());
    }
    std::string contents;
    in.seekg(0, std::ios::end);
    contents.resize(in.tellg());
    in.seekg(0, std::ios::beg);
    in.read(&contents[0], contents.size());
    in.close();
    return contents;
}

class Scorer
{
public:
    Scorer(const std::vector<ScoreMatrix>& matrices,
           const std::string& fasta,
           std::ofstream& output,
           std::mutex& output_mutex)
    :
        matrices(matrices),
        fasta(fasta),
        output(output),
        output_mutex(output_mutex)
    {}

    Scorer(const Scorer& other)
    :
        matrices(other.matrices),
        fasta(other.fasta),
        output(other.output),
        output_mutex(other.output_mutex)
    {}

    ~Scorer()
    {
        std::lock_guard<std::mutex> lock(output_mutex);

        output << ss.str();
    }

    Scorer& operator=(const Scorer& other) = delete;

    void score(size_t region_begin, const size_t region_end)
    {
        while(true)
        {
            // Note: Is > a valid char in a sequence name? maybe we should search for "\n>" char* instead of single char
            //       If this is needed, this could complicate finding the first start in the file.
            //       Will wait until have test case to validate this need.
            size_t name_begin = fasta.find_first_of('>', region_begin);
            if (name_begin >= region_end)
            {
                break;
            }
            name_begin++; // name starts character after '>'
            size_t name_end = fasta.find_first_of('\n', name_begin+1);

            size_t sequence_begin = name_end + 1;
            // this assumes the file is terminated by a newline, otherwise last sequence won't have an end
            size_t sequence_end = fasta.find_first_of('\n', sequence_begin + 1);

            for (const auto& matrix : matrices)
            {
                for (size_t begin = sequence_begin, end = sequence_begin + matrix.matrix().size();
                     end <= sequence_end;
                     ++begin, ++end)
                {
                    // Note: swapping loop order so loop by matrix then by sequence seems slightly slower in one test.
                    //       This is complicated also by motifs potentially having different lengths,
                    //       since the sequence substring length being dicated by the motif length.
                    const unsigned scaled_score = detail::score(matrix.matrix(),
                                                                fasta,
                                                                begin,
                                                                end);
                    const auto& pvalues = matrix.pvalues();
                    assert(scaled_score < pvalues.size());
                    const double pvalue = pvalues[scaled_score];
                    if (pvalue < 0.001)
                    {
                        const size_t name_length = name_end-name_begin;
                        const size_t sequence_start = begin - sequence_begin + 1;
                        const size_t sequence_stop = end - sequence_begin;
                        const double unscaled_score = double(scaled_score)/matrix.scale()
                            + matrix.matrix().size()*matrix.min_before_scaling();
                        ss << matrix.name()<< '\t';
                        ss.write(fasta.data() + name_begin, name_length);
                        ss << '\t'
                           << sequence_start << '\t'
                           << sequence_stop << '\t'
                           << (matrix.is_reverse_complement() ? '-' : '+') << '\t';

                        ss.precision(6);
                        ss << unscaled_score << '\t';
                        ss.precision(3);

                        ss << pvalue << '\t'
                           << '\t'; // omit q-value for now

                        if (matrix.is_reverse_complement())
                        {
                            // Note: might want to do toupper here and in the else (which is done in score_matrix.h).
                            //       Will wait until have test case to validate this need.
                            for (size_t i=begin + matrix.matrix().size() - 1; ; --i)
                            {
                                ss << complement(fasta[i]);
                                if (i==begin)
                                {
                                    break;
                                }
                            }
                        }
                        else
                        {
                            ss.write(fasta.data() + begin,
                                     matrix.matrix().size());
                        }

                        ss << std::endl;
                    }
                }
            }

            region_begin = sequence_end + 1;
        }
    }

private:
    const std::vector<ScoreMatrix>& matrices;
    const std::string& fasta;

    std::stringstream ss;

    std::ofstream& output;
    std::mutex& output_mutex;
};

typedef tbb::enumerable_thread_specific<Scorer,
                                        tbb::cache_aligned_allocator<Scorer>,
                                        tbb::ets_key_per_instance>
        Scorers;

void score_fasta(size_t region_begin, size_t region_end, Scorers& scorers)
{
    Scorer& scorer = scorers.local();
    scorer.score(region_begin, region_end);
}

void process_fasta(const std::vector<ScoreMatrix>& matrices,
                   const std::string& fasta_file_path,
                   const std::string& output_file_path)
{
    tbb::task_scheduler_init init(tbb::task_scheduler_init::automatic);

    #ifdef LIQUIDATOR_FASTA_SCORER_TIMINGS
    boost::timer::cpu_timer read_timer;
    #endif
    const std::string fasta = get_file_contents(fasta_file_path);
    #ifdef LIQUIDATOR_FASTA_SCORER_TIMINGS
    read_timer.stop();
    std::cout << "reading " << fasta.size() << " bytes fasta file into memory took" << read_timer.format() << std::endl;
    #endif
    // todo: instead of reading entire file into memory, just figure out how many char,
    //       then try having the scorers read the chunks into memory themselves.
    //       or maybe try the pipeline suggested in tbb documentation:
    //       https://www.threadingbuildingblocks.org/docs/help/tbb_userguide/Working_on_the_Assembly_Line_pipeline.html
    //       this is currently nice in its simplicity though.

    std::ofstream output(output_file_path);
    output << "#pattern name\tsequence name\tstart\tstop\tstrand\tscore\tp-value\tq-value\tmatched sequence" << std::endl;
    std::mutex output_mutex;

    Scorers scorers((Scorer(matrices, fasta, output, output_mutex)));

    // todo: follow instructions to pick proper grain size:
    // https://www.threadingbuildingblocks.org/docs/doxygen/a00023.html#a49a97576004711b7159170fcaf488e4e
    // actually, that documentation might be outdated... maybe grainsize of 1 fine because of auto_partitioner
    const size_t grainsize = 8400; // size of 100 fasta entries from a sample file

    #ifdef LIQUIDATOR_FASTA_SCORER_TIMINGS
    boost::timer::cpu_timer tbb_timer;
    #endif
    tbb::parallel_for(
      tbb::blocked_range<int>(0, fasta.size(), grainsize),
      [&](const tbb::blocked_range<int>& range)
      {
        score_fasta(range.begin(), range.end(), scorers);
      },
      tbb::auto_partitioner());
    #ifdef LIQUIDATOR_FASTA_SCORER_TIMINGS
    tbb_timer.stop();
    std::cout << "tbb // scoring took" << tbb_timer.format() << std::endl;
    #endif
}

}

/* The MIT License (MIT)

   Copyright (c) 2016 Boulder Labs (jdimatteo@boulderlabs.com)

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
 */
