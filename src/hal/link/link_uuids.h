/**
 * @file      link_uuids.h
 * @brief     PagerLink v1 GATT UUID constants — frozen.
 *
 * These UUIDs are frozen: they are mirrored in
 * ios/PagerKit/Sources/PagerKit/Link/UUIDs.swift and normative in
 * protocol/PROTOCOL.md §1. A change here lands together with the Swift
 * constant and the spec, in one commit.
 */
#pragma once

namespace pagerlink {

/** PagerLink primary service; advertised in the scan response. */
constexpr const char *SERVICE_UUID = "5BFF3035-4B08-4B35-BEEB-6A426D451D47";
/** Phone -> pager characteristic (Write / Write Without Response). */
constexpr const char *RX_UUID = "02855CCC-F4A4-4AA7-AFDC-237158247D4D";
/** Pager -> phone characteristic (Notify). */
constexpr const char *TX_UUID = "A975E68C-192B-4364-BE4E-6BB414A20346";
/** Read-only info characteristic (JSON identity, unencrypted). */
constexpr const char *INFO_UUID = "D7B0CF7E-0B2B-4CAC-BB23-4C0E20464B42";

}  // namespace pagerlink
