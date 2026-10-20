#pragma once

#include "CoreMinimal.h"

/** Per-frame image quality metrics. MSE/PSNR/SSIM use 8-bit sRGB; CNR uses linear luminance. */
struct FSLFrameMetrics
{
	int32 FrameIndex = 0;
	double MSE = 0.0;
	double PSNR = 0.0;
	double SSIM = 0.0;
	double CNR = 0.0;
};

/** Per-frame timing record. */
struct FSLFrameTiming
{
	int32 FrameIndex = 0;
	double RenderTimeMs = 0.0;
};

/** Summary row for one method run. */
struct FSLMethodSummary
{
	FString Method;
	FString RunId;
	double AvgRenderMs = 0.0;
	double AvgMSE = 0.0;
	double AvgPSNR = 0.0;
	double AvgSSIM = 0.0;
	double AvgCNR = 0.0;
};

/**
 * Stateless utility class for image quality metric computation and CSV I/O.
 * All image comparisons operate on tone-mapped 8-bit sRGB PNGs.
 */
class FSLFlythroughMetrics
{
public:
	/** Load a PNG file into an 8-bit RGBA pixel buffer. Returns false on failure. */
	static bool LoadPNG(const FString& Path, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight);

	/** Compute full-reference metrics. PSNR caps exact matches at 100 dB;
	 * CNR is the linear luminance std-dev ratio with a 1e-8 residual floor.
	 * Both images must have identical dimensions. */
	static FSLFrameMetrics ComputeFrameMetrics(int32 FrameIndex, const TArray<FColor>& TestPixels, const TArray<FColor>& GTPixels, int32 Width, int32 Height);

	/** Write per-frame timings to a CSV (with an avg summary row appended). */
	static bool WriteTimingsCSV(const FString& Path, const TArray<FSLFrameTiming>& Timings);

	/** Write per-frame metrics to a CSV (with an avg summary row appended). */
	static bool WriteMetricsCSV(const FString& Path, const TArray<FSLFrameMetrics>& Metrics);

	/** Append or update summary_metrics.csv in the renders root. */
	static bool WriteSummaryCSV(const FString& RendersRoot, const TArray<FSLMethodSummary>& Summaries);

	/** Read summary_metrics.csv. */
	static bool ReadSummaryCSV(const FString& RendersRoot, TArray<FSLMethodSummary>& OutSummaries);

	/** Read timings.csv to extract average render time. */
	static double ReadAvgRenderTime(const FString& TimingsPath);

	/** Find the latest complete run_NNN folder, ignoring cancelled/partial runs. */
	static FString FindLatestRun(const FString& MethodDir);

	/** Get the next run_NNN id for a method directory. */
	static FString GetNextRunId(const FString& MethodDir);

	/** Check whether a run directory has a complete set of frames. */
	static bool IsRunComplete(const FString& RunDir);

private:
	/** Compute SSIM for a single-channel luminance pair using 11x11 windows. */
	static double ComputeSSIM(const TArray<double>& TestLum, const TArray<double>& GTLum, int32 Width, int32 Height);

	/** Encoded sRGB luma [0, 255], used by SSIM. */
	static double ToLuminance(const FColor& C);
	/** Linear relative luminance [0, 1], used by CNR. */
	static double ToLinearLuminance(const FColor& C);
};
