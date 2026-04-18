#pragma once
#include <string>

enum class FitMode    { Fit, Fill, FillDown, Force };
enum class Gravity    { Attention, Entropy, Center };
enum class OutputFormat { WebP, JPEG, PNG, AVIF };

struct ResizeRequest {
    std::string  bucket;
    std::string  key;
    int          w       = 0;
    int          h       = 0;
    int          quality = -1;   // -1 = use per-format default
    FitMode      fit     = FitMode::Fit;
    OutputFormat output  = OutputFormat::WebP;
};

struct CropRequest {
    std::string  bucket;
    std::string  key;
    int          w       = 0;
    int          h       = 0;
    int          quality = -1;
    Gravity      gravity = Gravity::Attention;
    OutputFormat output  = OutputFormat::WebP;
};

struct ConvertRequest {
    std::string  bucket;
    std::string  key;
    int          quality = -1;
    OutputFormat output  = OutputFormat::WebP;
};
