// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/ip/aes/model/aes_modes.h"
#include "sw/device/lib/arch/boot_stage.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_aes.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_base.h"
#include "sw/device/lib/dif/dif_csrng.h"
#include "sw/device/lib/dif/dif_edn.h"
#include "sw/device/lib/dif/dif_entropy_src.h"
#include "sw/device/lib/dif/dif_keymgr.h"
#include "sw/device/lib/dif/dif_otbn.h"
#include "sw/device/lib/dif/dif_pwrmgr.h"
#include "sw/device/lib/dif/dif_rv_core_ibex.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/aes_testutils.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/keymgr_testutils.h"
#include "sw/device/lib/testing/otbn_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/tests/otbn_randomness_impl.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/dif/autogen/dif_entropy_src_autogen.h"

#define TIMEOUT (1000 * 1000)

OTTF_DEFINE_TEST_CONFIG();

// Module handles
static dif_entropy_src_t entropy_src;
static dif_csrng_t csrng;
static dif_edn_t edn0;
static dif_edn_t edn1;
static dif_aes_t aes;
static dif_otbn_t otbn;
static dif_alert_handler_t alert_handler;
static dif_keymgr_t keymgr;
dif_kmac_t kmac;

status_t init_test_environment(void) {
  LOG_INFO(
      "Initializing modules sntropy_src, csrng, edn0, edn1, aes, otbn and "
      "alert_handler...");
  TRY(dif_entropy_src_init(
      mmio_region_from_addr(TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR), &entropy_src));
  TRY(dif_csrng_init(mmio_region_from_addr(TOP_EARLGREY_CSRNG_BASE_ADDR),
                     &csrng));
  TRY(dif_edn_init(mmio_region_from_addr(TOP_EARLGREY_EDN0_BASE_ADDR), &edn0));
  TRY(dif_edn_init(mmio_region_from_addr(TOP_EARLGREY_EDN1_BASE_ADDR), &edn1));
  TRY(dif_aes_init(mmio_region_from_addr(TOP_EARLGREY_AES_BASE_ADDR), &aes));
  TRY(dif_otbn_init(mmio_region_from_addr(TOP_EARLGREY_OTBN_BASE_ADDR), &otbn));
  TRY(dif_keymgr_init(mmio_region_from_addr(TOP_EARLGREY_KEYMGR_BASE_ADDR),
                      &keymgr));
  TRY(dif_alert_handler_init(
      mmio_region_from_addr(TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR),
      &alert_handler));
  return OK_STATUS();
}

// Function to disable entropy complex
status_t disable_entropy_complex(void) {
  // Using entropy test utility to stop all EDN0, EDN1, CSRNG, and the Entropy
  // Source
  LOG_INFO("Disabling the entropy complex...");
  return entropy_testutils_stop_all();
}

status_t configure_realistic_fips_health_tests(void) {
  LOG_INFO("Configuring loose health test thresholds...");

  // Check if entropy source is locked
  bool is_locked;
  TRY(dif_entropy_src_is_locked(&entropy_src, &is_locked));
  TRY_CHECK(!is_locked,
            "Entropy source is locked. Cannot configure ENTROPY_SRC");

  // Configure Repetition Count Test
  dif_entropy_src_health_test_config_t repcnt_test_config = {
      .test_type = kDifEntropySrcTestRepetitionCount,
      .high_threshold = 512,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, repcnt_test_config));

  // Configure Adaptive Proportion Test
  dif_entropy_src_health_test_config_t adaptp_test_config = {
      .test_type = kDifEntropySrcTestAdaptiveProportion,
      .high_threshold = 512,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, adaptp_test_config));

  // Configure Bucket Test
  dif_entropy_src_health_test_config_t bucket_test_config = {
      .test_type = kDifEntropySrcTestBucket,
      .high_threshold = 512,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, bucket_test_config));

  // Configure Markov Test
  dif_entropy_src_health_test_config_t markov_test_config = {
      .test_type = kDifEntropySrcTestMarkov,
      .high_threshold = 512,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, markov_test_config));

  return OK_STATUS();
}

status_t enable_realistic_entropy_src_fips_mode(
    uint16_t health_test_window_size) {
  LOG_INFO("Enabling ENTROPY_SRC in fips mode...");

  // Ensure the entropy source is not locked
  bool is_locked;
  TRY(dif_entropy_src_is_locked(&entropy_src, &is_locked));
  TRY_CHECK(!is_locked,
            "Entropy source is locked. Cannot configure ENTROPY_SRC");

  // Configure ENTROPY_SRC in fips mode with health tests enabled
  dif_entropy_src_config_t config = {
      .fips_enable = true,
      .route_to_firmware = false,
      .fips_flag = true,
      .rng_fips = true,
      .bypass_conditioner = false,
      .single_bit_mode = kDifEntropySrcSingleBitModeDisabled,
      .health_test_threshold_scope = true,
      .health_test_window_size = health_test_window_size,
      .alert_threshold = 0xFF,
  };

  // Apply the configuration and enable ENTROPY_SRC
  TRY(dif_entropy_src_configure(&entropy_src, config, kDifToggleEnabled));

  return OK_STATUS();
}

status_t enable_csrng_edns_auto_mode(void) {
  LOG_INFO("Enabling EDNs in auto mode...");
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  // TRY(dif_edn_set_boot_mode(&edn0));
  // TRY(dif_edn_set_boot_mode(&edn1));
  // TRY(dif_edn_configure(&edn0));
  // TRY(dif_edn_configure(&edn1));
  return OK_STATUS();
}

status_t entropy_testutils_error_check_b4_proceeding(void) {
  LOG_INFO("Debugging error checks b4 proceeding...");
  return entropy_testutils_error_check(&entropy_src, &csrng, &edn0, &edn1);
}

status_t test_and_verify_aes_operation(void) {
  LOG_INFO("Triggering AES operation in ECB mode...");

  // Setup ECB encryption transaction
  dif_aes_transaction_t transaction = {
      .operation = kDifAesOperationEncrypt,
      .mode = kDifAesModeEcb,
      .key_len = kDifAesKey256,
      .key_provider = kDifAesKeySoftwareProvided,
      .mask_reseeding = kDifAesReseedPerBlock,
      .manual_operation = kDifAesManualOperationAuto,
      .reseed_on_key_change = false,
      .ctrl_aux_lock = false,
  };

  // Configure encryption
  CHECK_STATUS_OK(aes_testutils_setup_encryption(transaction, &aes));

  // Wait for the encryption to complete
  AES_TESTUTILS_WAIT_FOR_STATUS(&aes, kDifAesStatusOutputValid, true, TIMEOUT);

  // Decrypt and verify
  CHECK_STATUS_OK(aes_testutils_decrypt_ciphertext(transaction, &aes));

  LOG_INFO("AES operation in ECB mode verified successfully");
  return OK_STATUS();
}

status_t set_threshold_and_enable_stringent_entropy_src_fips_mode(void) {
  LOG_INFO("Enabling stringent ENTROPY_SRC in fips mode...");

  // Ensure the entropy source is not locked
  bool is_locked;
  TRY(dif_entropy_src_is_locked(&entropy_src, &is_locked));
  TRY_CHECK(!is_locked,
            "Entropy source is locked. Cannot configure ENTROPY_SRC");

  // Configure ENTROPY_SRC in fips mode with health tests enabled
  dif_entropy_src_config_t config = {
      .fips_enable = true,
      .route_to_firmware = false,
      .fips_flag = true,
      .rng_fips = true,
      .bypass_conditioner = false,
      .single_bit_mode = kDifEntropySrcSingleBitModeDisabled,
      .health_test_threshold_scope = false,
      .health_test_window_size = 512,
      .alert_threshold = 1,
  };

  // Apply the configuration and enable ENTROPY_SRC
  TRY(dif_entropy_src_configure(&entropy_src, config, kDifToggleEnabled));
  uint32_t errors;
  TRY(dif_entropy_src_get_errors(&entropy_src, &errors));
  LOG_INFO("ENTROPY_SRC Errors: 0x%x", errors);

  dif_entropy_src_irq_state_snapshot_t irq_state;
  TRY(dif_entropy_src_irq_get_state(&entropy_src, &irq_state));
  LOG_INFO("ENTROPY_SRC IRQ State: 0x%x", irq_state);

  return OK_STATUS();
}

status_t test_and_verify_otbn_operation(void) {
  LOG_INFO("Starting OTBN randomness test...");

  // Start the OTBN randomness test with one iteration
  otbn_randomness_test_start(&otbn, 1);
  busy_spin_micros(9500);

  // Wait for a timeout period to check if OTBN is still busy
  const uint32_t kIterateMaxRetries = 10;
  bool otbn_busy = true;
  uint32_t iter_cntr = kIterateMaxRetries;

  dif_otbn_status_t otbn_status;

  while (iter_cntr > 0) {
    // Check if OTBN is still busy
    TRY(dif_otbn_get_status(&otbn, &otbn_status));

    // Check if any of the busy status flags are set
    otbn_busy = (otbn_status &
                 (kDifOtbnStatusBusyExecute | kDifOtbnStatusBusySecWipeDmem |
                  kDifOtbnStatusBusySecWipeImem)) != 0;
    TRY_CHECK(!otbn_busy, "OTBN program completed as expected");
    iter_cntr--;
  }

  // After timeout, if OTBN is not busy, we shall conclude it's
  // completed running
  if (!otbn_busy) {
    // Print OTBN status and error bits
    dif_otbn_err_bits_t otbn_err_bits;
    TRY(dif_otbn_get_err_bits(&otbn, &otbn_err_bits));
    LOG_INFO("OTBN status: 0x%x", otbn_status);
    LOG_INFO("OTBN error bits: 0x%x", otbn_err_bits);

    LOG_INFO("OTBN program ran as expected with no hang");

    // Double check to confirm no other unexpected errors are
    // present leading to hang
    if (otbn_err_bits != kDifOtbnErrBitsNoError) {
      LOG_ERROR("OTBN encountered unexpected errors");

      // Optionally, decode and print specific error bits
      if (otbn_err_bits & kDifOtbnErrBitsBadDataAddr) {
        LOG_ERROR("A BAD_DATA_ADDR error was observed");
      }
      if (otbn_err_bits & kDifOtbnErrBitsBadInsnAddr) {
        LOG_ERROR("A BAD_INSN_ADDR error was observed");
      }
      if (otbn_err_bits & kDifOtbnErrBitsCallStack) {
        LOG_ERROR("A CALL_STACK error was observed");
      }
      if (otbn_err_bits & kDifOtbnErrBitsIllegalInsn) {
        LOG_ERROR("An ILLEGAL_INSN error was observed");
      }
      if (otbn_err_bits & kDifOtbnErrBitsLoop) {
        LOG_ERROR("A LOOP error was observed");
      }

      otbn_randomness_test_end(&otbn, 1);
      return INTERNAL();
    }
    return OK_STATUS();
  } else {
    LOG_ERROR("OTBN program did not complete run");
    return INTERNAL();
  }
}

status_t test_and_verify_keymgr_operation(void) {
  LOG_INFO("Triggering Keymgr operation...");

  // Initialize Keymgr (it uses entropy through CSRNG)
  CHECK_STATUS_OK(keymgr_testutils_initialize(&keymgr, &kmac));

  // Prepare parameters for advancing Keymgr state. For simplicity, use empty
  // bindings/salts.
  const dif_keymgr_state_params_t params = {
      .binding_value = {0},
  };
  CHECK_STATUS_OK(keymgr_testutils_advance_state(&keymgr, &params));

  // Check that Keymgr reached the INITIALIZED state.
  CHECK_STATUS_OK(
      keymgr_testutils_check_state(&keymgr, kDifKeymgrStateInitialized));

  LOG_INFO("Keymgr operation verified successfully");
  return OK_STATUS();
}

status_t configure_stringent_fips_health_tests(void) {
  LOG_INFO("Configuring stringent health test thresholds...");

  // Ensure the entropy source is not locked
  bool is_locked;
  TRY(dif_entropy_src_is_locked(&entropy_src, &is_locked));
  TRY_CHECK(!is_locked,
            "Entropy source is locked. Cannot configure ENTROPY_SRC");
  //.high_threshold = 0x1FF, --> passing for all health parameters
  //.high_threshold = 0x1,  --> EDN stucks in infinite loop for all health
  // parameters .high_threshold = 0x2,  --> EDN stucks in infinite loop for all
  // health parameters .high_threshold = 0xf,  --> EDN stucks in infinite loop
  // for all health parameters .high_threshold = 0xff,  --> EDN stucks in
  // infinite loop for all health parameters .high_threshold = 0x10F,  --> EDN
  // stucks in infinite loop for all health parameters .high_threshold = 0x1F0,
  //--> passing for all health parameters .high_threshold = 0x10A  --> EDN
  // stucks in infinite loop, for all health parameters .high_threshold = 0x1FA,
  //--> passing for all health parameters .high_threshold = 0x10E  --> EDN
  // stucks in infinite loop, for all health parameters

  // Configure Repetition Count Test with stringent threshold
  dif_entropy_src_health_test_config_t repcnt_test_config = {
      .test_type = kDifEntropySrcTestRepetitionCount,
      .high_threshold = 0x1F0,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, repcnt_test_config));

  // Configure Adaptive Proportion Test with stringent threshold
  dif_entropy_src_health_test_config_t adaptp_test_config = {
      .test_type = kDifEntropySrcTestAdaptiveProportion,
      //.high_threshold = 0xff,
      .high_threshold = 0x1F0,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, adaptp_test_config));

  // Configure Bucket Test with stringent threshold
  dif_entropy_src_health_test_config_t bucket_test_config = {
      .test_type = kDifEntropySrcTestBucket,
      //.high_threshold = 0xff,
      .high_threshold = 0x1F0,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, bucket_test_config));

  // Configure Markov Test with stringent threshold
  dif_entropy_src_health_test_config_t markov_test_config = {
      .test_type = kDifEntropySrcTestMarkov,
      //.high_threshold = 0xff,
      .high_threshold = 0x1F0,
      .low_threshold = 0,
  };
  TRY(dif_entropy_src_health_test_configure(&entropy_src, markov_test_config));

  return OK_STATUS();
}

status_t verify_recoverable_alert(void) {
  LOG_INFO("Verifying recoverable alerts...");

  // Log health test statistics
  dif_entropy_src_health_test_stats_t stats;
  TRY(dif_entropy_src_get_health_test_stats(&entropy_src, &stats));

  // Log alert fail test statistics
  dif_entropy_src_alert_fail_counts_t fail_counts;
  TRY(dif_entropy_src_get_alert_fail_counts(&entropy_src, &fail_counts));
  for (uint32_t i = 0; i < kDifEntropySrcTestNumVariants; i++) {
    if (i != 1 && i != 3 && i != 4 && i != 5) {
      continue;
    }
    uint32_t total_fails = stats.high_fails[i] + stats.low_fails[i];

    LOG_INFO(
        "Before Alerts: Test %u: high_watermark=%u low_watermark=%u "
        "high_fails=%u low_fails=%u total_fails=%u",
        i, stats.high_watermark[i], stats.low_watermark[i], stats.high_fails[i],
        stats.low_fails[i], total_fails);

    LOG_INFO("Alert Fail Counts for Test %u: HighFails=%u LowFails=%u", i,
             fail_counts.high_fails[i], fail_counts.low_fails[i]);
  }
  // Retrieve the recoverable alerts
  uint32_t alerts;
  TRY(dif_entropy_src_get_recoverable_alerts(&entropy_src, &alerts));

  if (alerts != 0) {
    LOG_INFO("Recoverable alert detected. Alerts: 0x%x", alerts);

    // Define alert bit masks based on documentation
    const uint32_t kEsMainSmAlertBit = 1 << 12;
    const uint32_t kEsThreshCfgAlertBit = 1 << 14;

    // Debug check if ES_THRESH_CFG_ALERT is set
    TRY_CHECK((alerts & kEsThreshCfgAlertBit) == 0,
              "ES_THRESH_CFG_ALERT is asserted");
    LOG_INFO("ES_THRESH_CFG_ALERT is not asserted and expected");

    // Check if ES_MAIN_SM_ALERT is set
    TRY_CHECK((alerts & kEsMainSmAlertBit) != 0,
              "ES_MAIN_SM_ALERT is not asserted when expected");
    LOG_INFO("ES_MAIN_SM_ALERT is asserted as expected");

    // Clear the recoverable alerts
    TRY(dif_entropy_src_clear_recoverable_alerts(&entropy_src, alerts));

    // Verify that the alerts have been cleared
    TRY(dif_entropy_src_get_recoverable_alerts(&entropy_src, &alerts));

    TRY_CHECK(alerts == 0, "Recoverable alerts not cleared. Alerts: 0x%x",
              alerts);
    LOG_INFO("Recoverable alerts successfully cleared");

    // Acknowledge the Health Test Failed interrupt
    TRY(dif_entropy_src_irq_acknowledge(&entropy_src,
                                        kDifEntropySrcIrqEsHealthTestFailed));

    // Check the IRQ state again
    dif_entropy_src_irq_state_snapshot_t irq_state;
    TRY(dif_entropy_src_irq_get_state(&entropy_src, &irq_state));
    LOG_INFO("ENTROPY_SRC IRQ State after acknowledging: 0x%x", irq_state);

    // Log health test statistics
    dif_entropy_src_health_test_stats_t stats;
    TRY(dif_entropy_src_get_health_test_stats(&entropy_src, &stats));

    // Log alert fail test statistics
    dif_entropy_src_alert_fail_counts_t fail_counts;
    TRY(dif_entropy_src_get_alert_fail_counts(&entropy_src, &fail_counts));
    for (uint32_t i = 0; i < kDifEntropySrcTestNumVariants; i++) {
      if (i != 1 && i != 3 && i != 4 && i != 5) {
        continue;
      }
      uint32_t total_fails = stats.high_fails[i] + stats.low_fails[i];

      LOG_INFO(
          "After Alerts: Test %u: high_watermark=%u low_watermark=%u "
          "high_fails=%u low_fails=%u total_fails=%u",
          i, stats.high_watermark[i], stats.low_watermark[i],
          stats.high_fails[i], stats.low_fails[i], total_fails);

      LOG_INFO("Alert Fail Counts for Test %u: HighFails=%u LowFails=%u", i,
               fail_counts.high_fails[i], fail_counts.low_fails[i]);
    }

    return OK_STATUS();
  } else {
    LOG_ERROR("No recoverable alerts detected when expected");
    return INTERNAL();
  }
}

status_t execute_test(void) {
  // Initialize peripherals and test environments
  CHECK_STATUS_OK(init_test_environment());

  // Step 8:  Repeat the procedure with different health test window sizes for
  // FIPS mode.
  uint16_t window_sizes[5] = {256, 512, 1024, 2048, 4096};
  for (uint16_t i = 0; i < ARRAYSIZE(window_sizes); i++) {
    LOG_INFO("Testing health test window size value = %u", window_sizes[i]);
    // Step 1: Disable the entropy complex
    CHECK_STATUS_OK(disable_entropy_complex());

    // Step 2: Configure realistic health test threshold values for fips FIPS/CC
    // compliant mode mode
    CHECK_STATUS_OK(configure_realistic_fips_health_tests());

    // Step 3: Enable ENTROPY_SRC in FIPS mode
    CHECK_STATUS_OK(enable_realistic_entropy_src_fips_mode(window_sizes[i]));

    // Step 4: Enable CSRNG
    // Step 5: Enable both EDNs in auto request mode
    CHECK_STATUS_OK(enable_csrng_edns_auto_mode());

    // Check for any error in configuring the modules
    CHECK_STATUS_OK(entropy_testutils_error_check_b4_proceeding());

    // Step 6: Trigger the execution of a cryptographic hardware block to stess
    // test the entropy (e.g. AES, OTBN) to test EDN0 Step 7: Verify the entropy
    // consuming endpoint(e.g. AES, OTBN) finishes its operation
    CHECK_STATUS_OK(test_and_verify_aes_operation());
    CHECK_STATUS_OK(test_and_verify_aes_operation());
    CHECK_STATUS_OK(test_and_verify_aes_operation());
    CHECK_STATUS_OK(test_and_verify_otbn_operation());
    CHECK_STATUS_OK(test_and_verify_otbn_operation());
    CHECK_STATUS_OK(test_and_verify_otbn_operation());
    // CHECK_STATUS_OK(test_and_verify_keymgr_operation());
  }
  LOG_INFO(
      "Realistic Test successfully passed with different health test window "
      "sizes...");
  // Step 9: Disable the entropy complex again
  CHECK_STATUS_OK(disable_entropy_complex());

  // Step 10: Configure unrealistically stringent health test threshold values
  // for FIPS mode
  CHECK_STATUS_OK(configure_stringent_fips_health_tests());

  // Step 11: Configure a low alert threshold value in the ALERT_THRESHOLD
  // register Step 12: Enable ENTROPY_SRC in FIPS mode
  CHECK_STATUS_OK(set_threshold_and_enable_stringent_entropy_src_fips_mode());

  // Step 13: Enable CSRNG
  // Step 14: Enable both EDNs in auto request mode and with low values for the
  // generate length parameter to request fresh entropy more often
  // CHECK_STATUS_OK(enable_edns_boot_mode());
  CHECK_STATUS_OK(enable_csrng_edns_auto_mode());

  // Check for any error in configuring the modules
  CHECK_STATUS_OK(entropy_testutils_error_check_b4_proceeding());

  // Step 15: Trigger the execution of various cryptographic hardware blocks
  // consuming entropy to stress test the entropy complex
  for (uint16_t i = 0; i < 110; i++) {
    CHECK_STATUS_OK(test_and_verify_aes_operation());
    CHECK_STATUS_OK(test_and_verify_otbn_operation());
    // CHECK_STATUS_OK(test_and_verify_keymgr_operation());
  }

  // Step 16: Verify that ENTROPY_SRC triggers a recoverable alert and sets the
  // RECOV_ALERT_STS.ES_MAIN_SM_ALERT bit
  // CHECK_STATUS_OK(verify_recoverable_alert());
  CHECK_STATUS_OK(verify_recoverable_alert());

  // Step 17 Verify that various entropy consuming endpoints hang as ENTROPY_SRC
  // stops generating entropy after triggering the recoverable alert

  LOG_INFO("Entropy source fips mode health test completed");

  return OK_STATUS();
}

bool test_main(void) {
  LOG_INFO("Entering Entropy Source FIPS Mode Health Test");

  return status_ok(execute_test());
}
