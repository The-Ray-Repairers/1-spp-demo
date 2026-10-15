#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

/**
 * Per-frame image quality metrics. MSE/PSNR/SSIM/CNR use the 8-bit sRGB PNGs;
 * RelMSE/SMAPE use the linear-HDR EXRs; TemporalFlicker uses the EXRs in
 * display space and needs the previous frame (unset on frame 0).
 */
struct FSLFrameMetrics
{
	int32 FrameIndex = 0;
	double MSE = 0.0;
	double PSNR = 0.0;
	double SSIM = 0.0;
	double CNR = 0.0;
	TOptional<double> RelMSE;
	TOptional<double> SMAPE;
	TOptional<double> TemporalFlicker;
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
	/** Unset when the run or the reference has no complete HDR frames. */
	TOptional<double> AvgRelMSE;
	TOptional<double> AvgSMAPE;
	TOptional<double> AvgTemporalFlicker;
	/** Reference run these numbers were computed against, e.g. "64SPP_GT/run_002". */
	FString Reference;
};

/** Per-pixel HDR error metrics, matching the Monte Carlo denoising literature. */
struct FSLHDRErrors
{
	double RelMSE = 0.0;
	double SMAPE = 0.0;
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

	/** Fill a summary's HDR averages from per-frame metrics; returns false if the run had no HDR data. */
	static bool ComputeSummaryHDR(const TArray<FSLFrameMetrics>& Metrics, FSLMethodSummary& InOutSummary);

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

	// ---- HDR (frames_hdr/*.exr) ----

	/** Epsilon in the relMSE and SMAPE denominators (the usual 1e-2). */
	static constexpr double HDREpsilon = 1e-2;

	/** Load an EXR as linear RGB(A) float. Returns false on failure. */
	static bool LoadEXR(const FString& Path, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight);

	/**
	 * relMSE = mean over pixels and RGB of (x - y)^2 / (y^2 + eps)
	 * SMAPE  = mean over pixels and RGB of |x - y| / (|x| + |y| + eps)
	 * x = test, y = reference, both linear HDR. Arrays must be the same size.
	 */
	static FSLHDRErrors ComputeHDRErrors(const TArray<FLinearColor>& Test, const TArray<FLinearColor>& Reference);

	/**
	 * Display transform shared with neural-denoiser's eval_ue: max(x,0)*Exposure,
	 * Reinhard x/(1+x), gamma 1/2.2. Output is interleaved RGB floats.
	 */
	static void ToDisplay(const TArray<FLinearColor>& Linear, TArray<float>& OutRGB, double Exposure = 1.0);

	/**
	 * Temporal flicker: mean |(x_t - x_{t-1}) - (y_t - y_{t-1})| over display-space
	 * RGB (neural-denoiser metrics.flicker). The reference follows the same camera
	 * path, so its frame difference already contains the motion; no warping needed.
	 */
	static double ComputeTemporalFlicker(const TArray<float>& TestPrev, const TArray<float>& TestCur,
		const TArray<float>& RefPrev, const TArray<float>& RefCur);

	/** Write hdr_frames.csv (frame_idx,pre_exposure), which marks frames_hdr/ complete. */
	static bool WriteHDRManifest(const FString& RunDir, const TArray<float>& PreExposures);

	/** True when hdr_frames.csv lists FrameCount frames and every frames_hdr/frame_NNNN.exr exists. */
	static bool IsHDRRunComplete(const FString& RunDir, int32 FrameCount);

private:
	/** Compute SSIM for a single-channel luminance pair using 11x11 windows. */
	static double ComputeSSIM(const TArray<double>& TestLum, const TArray<double>& GTLum, int32 Width, int32 Height);

	/** Convert sRGB FColor to relative luminance [0, 255]. */
	static double ToLuminance(const FColor& C);
};
