// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/psbtwallet.h>

TransactionError FillPSBT(const CWallet* pwallet, PartiallySignedTransaction& psbtx, bool& complete, int sighash_type, bool sign, bool bip32derivs, size_t* n_signed)
{
    if (n_signed) {
        *n_signed = 0;
    }
    LOCK(pwallet->cs_wallet);
    // Get all of the previous transactions
    complete = true;
    for (unsigned int i = 0; i < psbtx.tx->vin.size(); ++i) {
        const CTxIn& txin = psbtx.tx->vin[i];
        PSBTInput& input = psbtx.inputs.at(i);

        if (PSBTInputSigned(input)) {
            continue;
        }

        // Verify input looks sane. This will check that we have at most one uxto, witness or non-witness.
        if (!input.IsSane()) {
            return TransactionError::INVALID_PSBT;
        }

        // If we have no utxo, grab it from the wallet.
        if (!input.non_witness_utxo && input.witness_utxo.IsNull()) {
            const uint256& txhash = txin.prevout.hash;
            const auto it = pwallet->mapWallet.find(txhash);
            if (it != pwallet->mapWallet.end()) {
                const CWalletTx& wtx = it->second;
                // We only need the non_witness_utxo, which is a superset of the witness_utxo.
                //   The signing code will switch to the smaller witness_utxo if this is ok.
                input.non_witness_utxo = wtx.tx;
            }
        }

        // Get the Sighash type
        if (sign && input.sighash_type > 0 && input.sighash_type != sighash_type) {
            return TransactionError::SIGHASH_MISMATCH;
        }

        // Get the scriptPubKey to know which SigningProvider to use
        CScript script;
        if (!input.witness_utxo.IsNull()) {
            script = input.witness_utxo.scriptPubKey;
        } else if (input.non_witness_utxo) {
            script = input.non_witness_utxo->vout[txin.prevout.n].scriptPubKey;
        } else {
            // There's no UTXO so we can just skip this now
            complete = false;
            continue;
        }
        SignatureData sigdata;
        input.FillSignatureData(sigdata);
        std::unique_ptr<SigningProvider> provider = pwallet->GetSigningProvider(script, sigdata);
        if (!provider) {
            complete = false;
            continue;
        }

        bool signed_one = SignPSBTInput(HidingSigningProvider(provider.get(), !sign, !bip32derivs), psbtx, i, sighash_type);
        complete &= signed_one;
        if (n_signed && (signed_one || !sign)) {
            // If sign is false, we assume that we _could_ sign as long as we got a SigningProvider successfully. This
            // will never have false negatives; it is hard to tell under what circumstances it could have false positives.
            (*n_signed)++;
        }
    }

    // Fill in the bip32 keypaths and redeemscripts for the outputs so that hardware wallets can identify change
    for (unsigned int i = 0; i < psbtx.tx->vout.size(); ++i) {
        const CTxOut& out = psbtx.tx->vout.at(i);
        std::unique_ptr<SigningProvider> provider = pwallet->GetSigningProvider(out.scriptPubKey);
        if (provider) {
            UpdatePSBTOutput(HidingSigningProvider(provider.get(), true, !bip32derivs), psbtx, i);
        }
    }

    return TransactionError::OK;
}
