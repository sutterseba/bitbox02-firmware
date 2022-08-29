// Copyright 2019 Shift Cryptosecurity AG
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdlib.h>

#include "btc_common.h"

#include <hardfault.h>
#include <keystore.h>
#include <memory/memory.h>
#include <rust/rust.h>
#include <util.h>
#include <wally_address.h>

bool btc_common_pkscript_from_multisig(
    const multisig_t* multisig,
    uint32_t keypath_change,
    uint32_t keypath_address,
    uint8_t* script_out,
    size_t* script_out_size)
{
    uint8_t pubkeys[MULTISIG_P2WSH_MAX_SIGNERS * EC_PUBLIC_KEY_LEN];

    for (size_t index = 0; index < multisig->xpubs_count; index++) {
        struct ext_key xpub = {0};
        if (bip32_key_unserialize(multisig->xpubs[index], sizeof(multisig->xpubs[index]), &xpub) !=
            WALLY_OK) {
            return false;
        }
        struct ext_key derived_cosigner_xpub = {0};
        const uint32_t keypath[2] = {keypath_change, keypath_address};
        if (bip32_key_from_parent_path(
                &xpub, keypath, 2, BIP32_FLAG_KEY_PUBLIC, &derived_cosigner_xpub) != WALLY_OK) {
            return false;
        }
        memcpy(
            &pubkeys[index * EC_PUBLIC_KEY_LEN], derived_cosigner_xpub.pub_key, EC_PUBLIC_KEY_LEN);
    }

    size_t written;
    if (wally_scriptpubkey_multisig_from_bytes(
            pubkeys,
            multisig->xpubs_count * EC_PUBLIC_KEY_LEN,
            multisig->threshold,
            WALLY_SCRIPT_MULTISIG_SORTED,
            script_out,
            *script_out_size,
            &written) != WALLY_OK) {
        return false;
    }
    if (written > *script_out_size) {
        // Double check since the function above sets written to script_len if the buffer was too
        // short.
        return false;
    }
    *script_out_size = written;

    return true;
}

bool btc_common_payload_from_multisig(
    const multisig_t* multisig,
    multisig_script_type_t script_type,
    uint32_t keypath_change,
    uint32_t keypath_address,
    uint8_t* output_payload,
    size_t* output_payload_size)
{
    uint8_t script[MAX_PK_SCRIPT_SIZE] = {0};
    size_t written = sizeof(script);
    if (!btc_common_pkscript_from_multisig(
            multisig, keypath_change, keypath_address, script, &written)) {
        return false;
    }

    // TODO: double check that the witness script must be <= 10,000 bytes /
    // 201 opCounts (consensus rule), resp. 3,600 bytes (standardness rule).
    // See https://bitcoincore.org/en/segwit_wallet_dev/.
    // Note that the witness script has an additional varint prefix.

    switch (script_type) {
    case MULTISIG_SCRIPT_TYPE_P2WSH:
        *output_payload_size = SHA256_LEN;
        return wally_sha256(script, written, output_payload, SHA256_LEN) == WALLY_OK;
    case MULTISIG_SCRIPT_TYPE_P2WSH_P2SH: {
        // script_sha256 contains the hash of the multisig redeem script as used in a P2WSH output.
        uint8_t script_sha256[SHA256_LEN] = {0};
        if (wally_sha256(script, written, script_sha256, sizeof(script_sha256)) != WALLY_OK) {
            return false;
        }
        // create the p2wsh output.
        uint8_t p2wsh_pkscript[WALLY_SCRIPTPUBKEY_P2WSH_LEN] = {0};
        if (wally_witness_program_from_bytes(
                script_sha256,
                sizeof(script_sha256),
                0,
                p2wsh_pkscript,
                sizeof(p2wsh_pkscript),
                &written) != WALLY_OK) {
            return false;
        }
        // hash the output script according to p2sh.
        *output_payload_size = HASH160_LEN;
        return wally_hash160(p2wsh_pkscript, written, output_payload, HASH160_LEN) == WALLY_OK;
    }
    default:
        return false;
    };
}
