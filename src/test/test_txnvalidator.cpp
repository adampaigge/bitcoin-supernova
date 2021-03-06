// Copyright (c) 2019 The Bitcoin SV developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "config.h"
#include "key.h"
#include "pubkey.h"
#include "test/test_bitcoin.h"
#include "txmempool.h"
#include "txn_validator.h"

#include <boost/test/unit_test.hpp>

namespace {
    std::vector<TxSource> vTxSources {
        TxSource::wallet,
        TxSource::rpc,
        TxSource::file,
        TxSource::p2p,
        TxSource::reorg,
        TxSource::unknown,
        TxSource::finalised
    };
    // An exception thrown by GetValueOut .
    bool GetValueOutException(const std::runtime_error &e) {
        static std::string expectedException("GetValueOut: value out of range");
        return expectedException == e.what();
    }
    // Support for P2P node.
    CService ip(uint32_t i) {
        struct in_addr s;
        s.s_addr = i;
        return CService(CNetAddr(s), Params().GetDefaultPort());
    }
    // Create scriptPubKey from a given key
    CScript GetScriptPubKey(CKey& key) {
        CScript scriptPubKey = CScript() << ToByteVector(key.GetPubKey())
                                         << OP_CHECKSIG;
        return scriptPubKey;
    }
    // Create a double spend txn
    CMutableTransaction CreateDoubleSpendTxn(CTransaction& fundTxn,
                                       CKey& key,
                                       CScript& scriptPubKey) {
        static uint32_t dummyLockTime = 0;
        CMutableTransaction spend_txn;
        spend_txn.nVersion = 1;
        spend_txn.nLockTime = ++dummyLockTime;
        spend_txn.vin.resize(1);
        spend_txn.vin[0].prevout = COutPoint(fundTxn.GetId(), 0);
        spend_txn.vout.resize(1);
        spend_txn.vout[0].nValue = 11 * CENT;
        spend_txn.vout[0].scriptPubKey = scriptPubKey;
        // Sign:
        std::vector<uint8_t> vchSig {};
        uint256 hash = SignatureHash(scriptPubKey, CTransaction(spend_txn), 0,
                                     SigHashType().withForkId(),
                                     fundTxn.vout[0].nValue);
        BOOST_CHECK(key.Sign(hash, vchSig));
        vchSig.push_back(uint8_t(SIGHASH_ALL | SIGHASH_FORKID));
        spend_txn.vin[0].scriptSig << vchSig;
        return spend_txn;
    }
    // Make N unique large (but rubbish) transactions
    std::vector<CMutableTransaction> MakeNLargeTxns(size_t nNumTxns,
                                                    CTransaction& fundTxn,
                                                    CScript& scriptPubKey) {
        std::vector<CMutableTransaction> res {};
        for (size_t i=0; i<nNumTxns; i++) {
            CMutableTransaction txn;
            txn.nVersion = 1;
            txn.vin.resize(1);
            txn.vin[0].prevout = COutPoint(fundTxn.GetId(), i);
            txn.vout.resize(1000);
            for(size_t j=0; j<1000; ++j) {
                txn.vout[j].nValue = 11 * CENT;
                txn.vout[j].scriptPubKey = scriptPubKey;
            }
            res.emplace_back(txn);
        }
        return res;
    }
    // Create N double spend txns from the given fund txn
    std::vector<CMutableTransaction> CreateNDoubleSpendTxns(size_t nSpendTxns,
                                                      CTransaction& fundTxn,
                                                      CKey& key,
                                                      CScript& scriptPubKey) {
        // Create txns spending the same coinbase txn.
        std::vector<CMutableTransaction> spends {};
        for (size_t i=0; i<nSpendTxns; i++) {
            spends.emplace_back(CreateDoubleSpendTxn(fundTxn, key, scriptPubKey));
        };
        return spends;
    }
    // Create txn input data for a given txn and source
    TxInputDataSPtr TxInputData(TxSource source,
                                CMutableTransaction& spend,
                                std::shared_ptr<CNode> pNode = nullptr) {
        // Return txn's input data
        return std::make_shared<CTxInputData>(
                   g_connman->GetTxIdTracker(), // a pointer to the TxIdTracker
                   MakeTransactionRef(spend),// a pointer to the tx
                   source,   // tx source
                   TxValidationPriority::normal, // tx validation priority
                   GetTime(),// nAcceptTime
                   false,    // mfLimitFree
                   Amount(0), // nAbsurdFee
                   pNode);   // pNode
    }
    // Create a vector with input data for a given txn and source
    std::vector<TxInputDataSPtr> TxInputDataVec(TxSource source,
                                                std::vector<CMutableTransaction>& spends,
                                                std::shared_ptr<CNode> pNode = nullptr) {
        std::vector<TxInputDataSPtr> vTxInputData {};
        // Get a pointer to the TxIdTracker.
        const TxIdTrackerSPtr& pTxIdTracker = g_connman->GetTxIdTracker();
        for (auto& elem : spends) {
            vTxInputData.
                emplace_back(
                    std::make_shared<CTxInputData>(
                        pTxIdTracker, // a pointer to the TxIdTracker
                        MakeTransactionRef(elem),  // a pointer to the tx
                        source,   // tx source
                        TxValidationPriority::normal, // tx validation priority
                        GetTime(),// nAcceptTime
                        false,    // mfLimitFree
                        Amount(0), // nAbsurdFee
                        pNode));   // pNode
        }
        return vTxInputData;
    }
    // Validate txn using asynchronous validation interface
    void ProcessTxnsAsynchApi(std::vector<CMutableTransaction>& spends,
                              TxSource source,
                              std::shared_ptr<CNode> pNode = nullptr) {
        // Create txn validator
        std::shared_ptr<CTxnValidator> txnValidator {
            std::make_shared<CTxnValidator>(
                    GlobalConfig::GetConfig(),
                    mempool,
                    std::make_shared<CTxnDoubleSpendDetector>(),
                    g_connman->GetTxIdTracker())
        };
        // Clear mempool before validation
        mempool.Clear();
        // Schedule txns for processing.
        txnValidator->newTransaction(TxInputDataVec(source, spends, pNode));
        // Wait for the Validator to process all queued txns.
        txnValidator->waitForEmptyQueue();
    }
    // Validate a single txn using synchronous validation interface
    CValidationState ProcessTxnSynchApi(CMutableTransaction& spend,
                                        TxSource source,
                                        std::shared_ptr<CNode> pNode = nullptr) {
        // Create txn validator
        std::shared_ptr<CTxnValidator> txnValidator {
            std::make_shared<CTxnValidator>(
                    GlobalConfig::GetConfig(),
                    mempool,
                    std::make_shared<CTxnDoubleSpendDetector>(),
                    g_connman->GetTxIdTracker())
        };
        // Clear mempool before validation
        mempool.Clear();
        // Mempool Journal ChangeSet
        mining::CJournalChangeSetPtr changeSet {nullptr};
        return txnValidator->processValidation(TxInputData(source, spend, pNode), changeSet);
    }
    // Validate txn using synchronous validation interface
    void ProcessTxnsSynchApi(std::vector<CMutableTransaction>& spends,
                             TxSource source,
                             std::shared_ptr<CNode> pNode = nullptr) {
        // Create txn validator
        std::shared_ptr<CTxnValidator> txnValidator {
            std::make_shared<CTxnValidator>(
                    GlobalConfig::GetConfig(),
                    mempool,
                    std::make_shared<CTxnDoubleSpendDetector>(),
                    g_connman->GetTxIdTracker())
        };
        // Clear mempool before validation
        mempool.Clear();
        // Mempool Journal ChangeSet
        mining::CJournalChangeSetPtr changeSet {nullptr};
        // Validate the first txn
        CValidationState result {
            txnValidator->processValidation(TxInputData(source, spends[0], pNode), changeSet)
        };
        BOOST_CHECK(result.IsValid());
        // Validate the second txn
        // spends1 should be rejected if spend0 is in the mempool
        result = txnValidator->processValidation(TxInputData(source, spends[1], pNode), changeSet);
        BOOST_CHECK(!result.IsValid());
    }
    // Validate txns using synchronous batch validation interface
    CTxnValidator::RejectedTxns ProcessTxnsSynchBatchApi(std::vector<CMutableTransaction>& spends,
                                  TxSource source,
                                  std::shared_ptr<CNode> pNode = nullptr) {
        // Create txn validator
        std::shared_ptr<CTxnValidator> txnValidator {
            std::make_shared<CTxnValidator>(
                    GlobalConfig::GetConfig(),
                    mempool,
                    std::make_shared<CTxnDoubleSpendDetector>(),
                    g_connman->GetTxIdTracker())
        };
        // Clear mempool before validation
        mempool.Clear();
        // Mempool Journal ChangeSet
        mining::CJournalChangeSetPtr changeSet {nullptr};
        // Validate the first txn
        return txnValidator->processValidation(TxInputDataVec(source, spends, pNode), changeSet);
    }
    struct TestChain100Setup2 : TestChain100Setup {
        CScript scriptPubKey {
            GetScriptPubKey(coinbaseKey)
        };
        // twoDoubleSpend2Txns contains two txns spending the same coinbase txn
        std::vector<CMutableTransaction> doubleSpend2Txns {
            CreateDoubleSpendTxn(coinbaseTxns[0], coinbaseKey, scriptPubKey),
            CreateDoubleSpendTxn(coinbaseTxns[0], coinbaseKey, scriptPubKey)
        };
        // doubleSpend10Txns contains 10 double spend txns spending the same coinbase txn
        std::vector<CMutableTransaction> doubleSpend10Txns {
            CreateNDoubleSpendTxns(10, coinbaseTxns[0], coinbaseKey, scriptPubKey)
        };    
    };
}

BOOST_FIXTURE_TEST_SUITE(test_txnvalidator, TestChain100Setup2)

BOOST_AUTO_TEST_CASE(txn_validator_creation) {
    // Create txn validator
    std::shared_ptr<CTxnValidator> txnValidator {
        std::make_shared<CTxnValidator>(
                GlobalConfig::GetConfig(),
                mempool,
                std::make_shared<CTxnDoubleSpendDetector>(),
                g_connman->GetTxIdTracker())
    };
    // Check if the Validator was created
    BOOST_REQUIRE(txnValidator);
    // Check if orphan txns buffer was created
    BOOST_REQUIRE(txnValidator->getOrphanTxnsPtr());
    // Check if txn recent rejects buffer was created
    BOOST_REQUIRE(txnValidator->getTxnRecentRejectsPtr());
}

BOOST_AUTO_TEST_CASE(txn_validator_set_get_frequency) {
    // Create txn validator
    std::shared_ptr<CTxnValidator> txnValidator {
        std::make_shared<CTxnValidator>(
                GlobalConfig::GetConfig(),
                mempool,
                std::make_shared<CTxnDoubleSpendDetector>(),
                g_connman->GetTxIdTracker())
    };
    auto defaultfreq = std::chrono::milliseconds(CTxnValidator::DEFAULT_ASYNCH_RUN_FREQUENCY_MILLIS);
    BOOST_CHECK(defaultfreq == txnValidator->getRunFrequency());
    txnValidator->setRunFrequency(++defaultfreq);
    BOOST_CHECK(defaultfreq == txnValidator->getRunFrequency());
}

BOOST_AUTO_TEST_CASE(txn_validator_istxnknown) {
    // Create txn validator
    std::shared_ptr<CTxnValidator> txnValidator {
        std::make_shared<CTxnValidator>(
                GlobalConfig::GetConfig(),
                mempool,
                std::make_shared<CTxnDoubleSpendDetector>(),
                g_connman->GetTxIdTracker())
    };
    // Schedule txns for processing.
    txnValidator->newTransaction(TxInputDataVec(TxSource::p2p, doubleSpend10Txns));
    BOOST_CHECK(txnValidator->isTxnKnown(doubleSpend10Txns[0].GetId()));
    // Wait for the Validator to process all queued txns.
    txnValidator->waitForEmptyQueue();
    BOOST_CHECK(!txnValidator->isTxnKnown(doubleSpend10Txns[0].GetId()));
}

/**
 * TxnValidator: Test synch interface.
 */
BOOST_AUTO_TEST_CASE(txnvalidator_doublespend_synch_api) {
    // Test all sources.
    for (const auto& txsource: vTxSources) {
        ProcessTxnsSynchApi(doubleSpend2Txns, txsource);
        BOOST_CHECK_EQUAL(mempool.Size(), 1);
    }
    // Test: Txns from p2p with a pointer to a dummy node.
    {
        // Create a dummy address
        CAddress dummy_addr(ip(0xa0b0c001), NODE_NONE);
        CConnman::CAsyncTaskPool asyncTaskPool{GlobalConfig::GetConfig()};
        CNodePtr pDummyNode =
            CNode::Make(
                0,
                NODE_NETWORK,
                0,
                INVALID_SOCKET,
                dummy_addr,
                0u,
                0u,
                asyncTaskPool,
                "",
                true);
        ProcessTxnsSynchApi(doubleSpend2Txns, TxSource::p2p, pDummyNode);
        BOOST_CHECK_EQUAL(mempool.Size(), 1);
    }
}

BOOST_AUTO_TEST_CASE(txnvalidator_doublespend_synch_batch_api) {
    CTxnValidator::RejectedTxns mRejectedTxns {};
    // Test all sources.
    for (const auto& txsource: vTxSources) {
        mRejectedTxns = ProcessTxnsSynchBatchApi(doubleSpend10Txns, txsource);
        BOOST_CHECK_EQUAL(mempool.Size(), 1);
        // There should be no insufficient fee txns returned.
        BOOST_REQUIRE(!mRejectedTxns.second.size());
        // Check an expected number of invalid txns returned.
        const CTxnValidator::InvalidTxnStateUMap& mInvalidTxns = mRejectedTxns.first;
        BOOST_REQUIRE(mInvalidTxns.size() == doubleSpend10Txns.size()-1);
        for (const auto& elem : mInvalidTxns) {
            BOOST_REQUIRE(!elem.second.IsValid());
            // Due to runtime-conditions it might be detected as:
            // - a mempool conflict
            // - a double spend
            BOOST_REQUIRE(elem.second.IsMempoolConflictDetected() ||
                elem.second.IsDoubleSpendDetected());
        }
    }
    // Test: Txns from p2p with a pointer to a dummy node.
    {
        // Create a dummy address
        CAddress dummy_addr(ip(0xa0b0c001), NODE_NONE);
        CConnman::CAsyncTaskPool asyncTaskPool{GlobalConfig::GetConfig()};
        CNodePtr pDummyNode =
            CNode::Make(
                0,
                NODE_NETWORK,
                0,
                INVALID_SOCKET,
                dummy_addr,
                0u,
                0u,
                asyncTaskPool,
                "",
                true);
        mRejectedTxns = ProcessTxnsSynchBatchApi(doubleSpend10Txns, TxSource::p2p, pDummyNode);
        BOOST_CHECK_EQUAL(mempool.Size(), 1);
        // There should be no insufficient fee txns returned.
        BOOST_REQUIRE(!mRejectedTxns.second.size());
        // Check an expected number of invalid txns returned.
        const CTxnValidator::InvalidTxnStateUMap& mInvalidTxns = mRejectedTxns.first;
        BOOST_REQUIRE(mInvalidTxns.size() == doubleSpend10Txns.size()-1);
        for (const auto& elem : mInvalidTxns) {
            BOOST_REQUIRE(!elem.second.IsValid());
            // Due to runtime-conditions it might be detected as:
            // - a mempool conflict
            // - a double spend
            BOOST_REQUIRE(elem.second.IsMempoolConflictDetected() ||
                elem.second.IsDoubleSpendDetected());
        }
    }
}

/**
 * TxnValidator: Test asynch interface.
 */
BOOST_AUTO_TEST_CASE(txnvalidator_wallet_doublespend_via_asynch_api) {
    // Test: Txns from wallet.
    ProcessTxnsAsynchApi(doubleSpend10Txns, TxSource::wallet);
    BOOST_CHECK_EQUAL(mempool.Size(), 1);
}

BOOST_AUTO_TEST_CASE(txnvalidator_rpc_doublespend_via_asynch_api) {
    // Test: Txns from rpc.
    ProcessTxnsAsynchApi(doubleSpend10Txns, TxSource::rpc);
    BOOST_CHECK_EQUAL(mempool.Size(), 1);
}

BOOST_AUTO_TEST_CASE(txnvalidator_file_doublespend_via_asynch_api) {
    // Test: Txns from file.
    ProcessTxnsAsynchApi(doubleSpend10Txns, TxSource::file);
    BOOST_CHECK_EQUAL(mempool.Size(), 1);
}

BOOST_AUTO_TEST_CASE(txnvalidator_p2p_doublespend_via_asynch_api) {
    // Test: Txns from p2p.
    {
        // Create a dummy address
        CAddress dummy_addr(ip(0xa0b0c001), NODE_NONE);
        CConnman::CAsyncTaskPool asyncTaskPool{GlobalConfig::GetConfig()};
        CNodePtr pDummyNode =
            CNode::Make(
                0,
                NODE_NETWORK,
                0,
                INVALID_SOCKET,
                dummy_addr,
                0u,
                0u,
                asyncTaskPool,
                "",
                true);
        ProcessTxnsAsynchApi(doubleSpend10Txns, TxSource::p2p, pDummyNode);
        BOOST_CHECK_EQUAL(mempool.Size(), 1);
    }
    // Process txn if it is valid.
    ProcessTxnsAsynchApi(doubleSpend10Txns, TxSource::p2p);
    BOOST_CHECK_EQUAL(mempool.Size(), 1);
}

BOOST_AUTO_TEST_CASE(txnvalidator_reorg_doublespend_via_asynch_api) {
    // Test: Txns from reorg.
    ProcessTxnsAsynchApi(doubleSpend10Txns, TxSource::reorg);
    BOOST_CHECK_EQUAL(mempool.Size(), 1);
}

BOOST_AUTO_TEST_CASE(txnvalidator_dummy_doublespend_via_asynch_api) {
    // Process txn if it is valid.
    ProcessTxnsAsynchApi(doubleSpend10Txns, TxSource::unknown);
    BOOST_CHECK_EQUAL(mempool.Size(), 1);
}

BOOST_AUTO_TEST_CASE(txnvalidator_limit_memory_usage)
{
    // Make sure validation thread won't run during this test
    gArgs.ForceSetArg("-txnvalidationasynchrunfreq", "10000");
    gArgs.ForceSetArg("-txnvalidationqueuesmaxmemory", "1");

    // Create a larger number of txns than will fit in a 1Mb queue
    std::vector<CMutableTransaction> txns { MakeNLargeTxns(25, coinbaseTxns[0], scriptPubKey) };
    auto txnsInputs { TxInputDataVec(TxSource::p2p, txns) };

    // Create txn validator
    std::shared_ptr<CTxnValidator> txnValidator {
        std::make_shared<CTxnValidator>(
                GlobalConfig::GetConfig(),
                mempool,
                std::make_shared<CTxnDoubleSpendDetector>(),
                g_connman->GetTxIdTracker())
    };

    // Attempt to enqueue all txns and verify that we stopped when we hit the max size limit
    txnValidator->newTransaction(txnsInputs);
    BOOST_CHECK(txnValidator->GetTransactionsInQueueCount() < txns.size());
    BOOST_CHECK(txnValidator->GetStdQueueMemUsage() <= 1*ONE_MEBIBYTE);
    BOOST_CHECK_EQUAL(txnValidator->GetNonStdQueueMemUsage(), 0);
}

BOOST_AUTO_TEST_CASE(txnvalidator_nvalueoutofrange_sync_api) {
    // spendtx_nValue_OutOfRange (a copy of doubleSpend2Txns[0]) with unsupported nValue amount.
    // Set nValue = MAX_MONEY + 1 for the txn to trigger exception when GetValueOut is called.
    auto spendtx_nValue_OutOfRange = doubleSpend2Txns[0];
    spendtx_nValue_OutOfRange.vout[0].nValue = MAX_MONEY + Amount(1);
    BOOST_CHECK_EXCEPTION(
        !MoneyRange(CTransaction(spendtx_nValue_OutOfRange).GetValueOut()),
        std::runtime_error,
        GetValueOutException);
    CValidationState result {};
    // Test all sources.
    for (const auto& txsource: vTxSources) {
        result = ProcessTxnSynchApi(spendtx_nValue_OutOfRange, txsource);
        BOOST_CHECK(!result.IsValid());
        BOOST_CHECK_EQUAL(mempool.Size(), 0);
    }
}

BOOST_AUTO_TEST_CASE(txnvalidator_nvalueoutofrange_async_api) {
    // Create txn validator
    std::shared_ptr<CTxnValidator> txnValidator {
        std::make_shared<CTxnValidator>(
                GlobalConfig::GetConfig(),
                mempool,
                std::make_shared<CTxnDoubleSpendDetector>(),
                g_connman->GetTxIdTracker())
    };
    // Case1:
    // doubleSpends10Txns_nValue_OutOfRange (a copy of doubleSpend10Txns) with unsupported nValue amount.
    {
        // Set nValue = MAX_MONEY + 1 for each txn to trigger exception when GetValueOut is called.
        auto doubleSpends10Txns_nValue_OutOfRange = doubleSpend10Txns;
        for (auto& spend: doubleSpends10Txns_nValue_OutOfRange) {
            spend.vout[0].nValue = MAX_MONEY + Amount(1);
            BOOST_CHECK_EXCEPTION(!MoneyRange(CTransaction(spend).GetValueOut()), std::runtime_error, GetValueOutException);
        }
        // Schedule txns for processing.
        txnValidator->newTransaction(TxInputDataVec(TxSource::p2p, doubleSpends10Txns_nValue_OutOfRange));
        // Wait for the Validator to process all queued txns.
        txnValidator->waitForEmptyQueue();
        // Non transaction should be accepted due to nValue (value out of range).
        BOOST_CHECK_EQUAL(mempool.Size(), 0);
    }
    // Case2:
    // Send the same txns again (with valid nValue).
    // Check if only one txn (from doubleSpend10Txns) is accepted by the mempool.
    {
        txnValidator->newTransaction(TxInputDataVec(TxSource::p2p, doubleSpend10Txns));
        txnValidator->waitForEmptyQueue();
        BOOST_CHECK_EQUAL(mempool.Size(), 1);
    }
}

BOOST_AUTO_TEST_SUITE_END()
