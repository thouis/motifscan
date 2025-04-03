#ifndef LIQUIDATOR_BAM_SCORER_H_INCLUDED
#define LIQUIDATOR_BAM_SCORER_H_INCLUDED

#include "bamliquidator_regions.h"
#include "score_matrix.h"

#include <boost/filesystem.hpp>

#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <deque>

static const int MAX_QUEUED_READS = 200;
static const int MAX_THREAD_CHUNK = 100000;

typedef void *bam1_t;
typedef void *bamFile;
typedef void *bam_header_t;
typedef void *bam_index_t;

namespace liquidator
{

class BamScorer
{   
public:
    enum PrintStyle
    {
        None,
        Fimo,
        MappedFimo
    };
};

}

#endif
