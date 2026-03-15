#pragma once
#include <cstdint>
#include <optional>
#include <string>

//ID is 32-bit for performance/memory efficiency.

#include "../../third_party/roaring_bitmap/roaring.hh"
#include "../utils/settings.hpp"

namespace ndd {

    enum class SparseScoringModel : uint8_t {
        DEFAULT = 0,
        ENDEE_BM25_SERVER_IDF = 1,
    };

    inline const char* sparseScoringModelToString(SparseScoringModel model) {
        switch(model) {
            case SparseScoringModel::DEFAULT:
                return "default";
            case SparseScoringModel::ENDEE_BM25_SERVER_IDF:
                return "endee_bm25_server_idf";
        }
        return "default";
    }

    inline std::optional<SparseScoringModel> sparseScoringModelFromString(
        const std::string& value)
    {
        if(value == "default") {
            return SparseScoringModel::DEFAULT;
        }
        if(value == "endee_bm25_server_idf") {
            return SparseScoringModel::ENDEE_BM25_SERVER_IDF;
        }
        return std::nullopt;
    }

    struct FilterParams {
        size_t prefilter_threshold = settings::PREFILTER_CARDINALITY_THRESHOLD;
        size_t boost_percentage = settings::FILTER_BOOST_PERCENTAGE;
    };

    using idInt = uint32_t;   // External ID (stored in DB, exposed to user)
    using idhInt = uint32_t;  // Internal HNSW ID (used inside HNSW structures)
    using RoaringBitmap = roaring::Roaring;

}  //namespace ndd
