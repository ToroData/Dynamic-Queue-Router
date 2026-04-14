/**
 * @file dmr_qcut_label.h
 * @brief Compiled default rules for labeling QCut artifacts in DMR
 *
 * This header defines the compiled-time default thresholds and
 * filesystem conventions used by the QCut labeling subsystem.
 *
 * The labeling classifies each QCut subcircuit into one of:
 *   - QC         (suitable for a QPU)
 *   - HPC        (preferable for classical/HPC execution)
 *   - Undecided  (insufficient or intermediate characteristics)
 *
 * The values defined here can be overridden at runtime using:
 *   1. Environment variables (highest priority)
 *   2. Optional configuration file pointed by `DMR_QCUT_LABEL_CONFIG`
 *   3. These compiled defaults (lowest priority)
 */
#ifndef DMR_QCUT_LABEL_H
#define DMR_QCUT_LABEL_H

#ifndef DMR_QCUT_QC_MAX_SIZE
#define DMR_QCUT_QC_MAX_SIZE 0
#endif

#ifndef DMR_QCUT_HPC_MIN_SIZE
#define DMR_QCUT_HPC_MIN_SIZE 0
#endif

/**
 * @def DMR_QCUT_OUTPUT_DIR_DEFAULT
 * @brief Default directory containing QCut output artifacts.
 *
 * This directory is expected to contain:
 *   - A `subcircuits/` subdirectory with per-subcircuit JSON metadata
 *   - Generated labeling outputs (CSV summary and label files)
 *
 * Can be overridden at runtime via the `DMR_QCUT_OUTPUT_DIR` environment
 * variable.
 */
#ifndef DMR_QCUT_OUTPUT_DIR_DEFAULT
#define DMR_QCUT_OUTPUT_DIR_DEFAULT "./output"
#endif

/**
 * @def DMR_QCUT_SUBDIR_DEFAULT
 * @brief Default subdirectory name containing QCut subcircuit artifacts.
 *
 * This directory is resolved relative to `DMR_QCUT_OUTPUT_DIR_DEFAULT`
 * unless an absolute path is provided via `DMR_QCUT_SUBCIRCUITS_DIR`.
 */
#ifndef DMR_QCUT_SUBDIR_DEFAULT
#define DMR_QCUT_SUBDIR_DEFAULT "subcircuits"
#endif

/**
 * @def DMR_QCUT_LABELS_CSV_DEFAULT
 * @brief Filename of the global CSV summary produced by the labeler.
 *
 * The CSV aggregates labeling decisions for all processed subcircuits
 * and is written under the output directory.
 */
#ifndef DMR_QCUT_LABELS_CSV_DEFAULT
#define DMR_QCUT_LABELS_CSV_DEFAULT "dmr_labels.csv"
#endif

/**
 * @def DMR_QCUT_LABEL_FILE_EXT
 * @brief Extension used for subcircuit label files.
 *
 * Each label file is written next to its corresponding subcircuit
 * metadata file and contains a simple key=value representation
 * of the labeling decision.
 */
#ifndef DMR_QCUT_LABEL_FILE_EXT
#define DMR_QCUT_LABEL_FILE_EXT ".dmr_label"
#endif

/**
 * @section qcut_label_heuristics Labeling heuristics
 *
 * The labeling process relies on coarse-grained heuristics derived from
 * execution constraints:
 *
 *   - QC: Subcircuits small enough to be realistically executed on
 *     current or near-term QPU hardware.
 *
 *   - HPC: Subcircuits large or deep enough to justify classical or
 *     HPC-based simulation, tensor contraction, or approximation.
 *
 *   - Undecided: Subcircuits that fall between these regimes or lack
 *     sufficient metadata for a confident decision.
 *
 * These thresholds are platform- and backend-dependent and should be
 * tuned accordingly.
 */

/**
 * @def DMR_QCUT_QC_MAX_QUBITS
 * @brief Maximum number of qubits allowed for QC classification.
 *
 * Subcircuits exceeding this value are not considered suitable for
 * direct QPU execution.
 */
#ifndef DMR_QCUT_QC_MAX_QUBITS
#define DMR_QCUT_QC_MAX_QUBITS 30
#endif

/**
 * @def DMR_QCUT_QC_MAX_DEPTH
 * @brief Maximum circuit depth allowed for QC classification.
 *
 * Acts as a proxy for accumulated noise and decoherence on QPU hardware.
 */
#ifndef DMR_QCUT_QC_MAX_DEPTH
#define DMR_QCUT_QC_MAX_DEPTH 200
#endif

/**
 * @def DMR_QCUT_QC_MAX_2Q
 * @brief Maximum number of two-qubit gates allowed for QC classification.
 */
#ifndef DMR_QCUT_QC_MAX_2Q
#define DMR_QCUT_QC_MAX_2Q 400
#endif

/* HPC thresholds */
/**
 * @def DMR_QCUT_QC_MAX_2Q
 * @brief Maximum number of two-qubit gates allowed for QC classification.
 */
#ifndef DMR_QCUT_HPC_MIN_QUBITS
#define DMR_QCUT_HPC_MIN_QUBITS 38
#endif

/**
 * @def DMR_QCUT_HPC_MIN_DEPTH
 * @brief Minimum circuit depth triggering an HPC classification.
 */
#ifndef DMR_QCUT_HPC_MIN_DEPTH
#define DMR_QCUT_HPC_MIN_DEPTH 350
#endif

/**
 * @def DMR_QCUT_HPC_MIN_2Q
 * @brief Minimum number of two-qubit gates triggering an HPC classification.
 */
#ifndef DMR_QCUT_HPC_MIN_2Q
#define DMR_QCUT_HPC_MIN_2Q 900
#endif

#endif /* DMR_QCUT_LABEL_H */
