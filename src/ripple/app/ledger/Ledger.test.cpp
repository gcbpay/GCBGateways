//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/consensus/LedgerConsensus.h>
#include <ripple/app/ledger/LedgerTiming.h>
#include <ripple/app/misc/CanonicalTXSet.h>
#include <ripple/app/transactors/Transactor.h>
#include <ripple/basics/seconds_clock.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/RippleAddress.h>
#include <ripple/protocol/STParsedJSON.h>
#include <ripple/protocol/TxFlags.h>
#include <beast/unit_test/suite.h>

namespace ripple {

class Ledger_test : public beast::unit_test::suite
{
    using TestAccount = std::pair<RippleAddress, unsigned>;

    struct Amount
    {
        Amount (double value_, std::string currency_, TestAccount issuer_)
            : value(value_)
            , currency(currency_)
            , issuer(issuer_)
        {
        }

        double value;
        std::string currency;
        TestAccount issuer;

        Json::Value
        getJson() const
        {
            Json::Value tx_json;
            tx_json["currency"] = currency;
            tx_json["issuer"] = issuer.first.humanAccountID();
            tx_json["value"] = std::to_string(value);
            return tx_json;
        }
    };

    // Helper function to parse a transaction in Json, sign it with account,
    // and return it as a STTx
    STTx
    parseTransaction(TestAccount& account, Json::Value const& tx_json)
    {
        STParsedJSONObject parsed("tx_json", tx_json);
        std::unique_ptr<STObject> sopTrans = std::move(parsed.object);
        expect(sopTrans != nullptr);
        sopTrans->setFieldVL(sfSigningPubKey, account.first.getAccountPublic());
        return STTx(*sopTrans);
    }

    // Helper function to apply a transaction to a ledger
    void
    applyTransaction(Ledger::pointer const& ledger, STTx const& tx)
    {
        TransactionEngine engine(ledger);
        bool didApply = false;
        auto r = engine.applyTransaction(tx, tapOPEN_LEDGER | tapNO_CHECK_SIGN,
                                         didApply);
        expect(r == tesSUCCESS);
        expect(didApply);
    }

    // Create genesis ledger from a start amount in drops, and the public
    // master RippleAddress
    Ledger::pointer
    createGenesisLedger(std::uint64_t start_amount_drops, TestAccount const& master)
    {
        Ledger::pointer ledger = std::make_shared<Ledger>(master.first,
                                                          start_amount_drops,
                                                          start_amount_drops);
        ledger->updateHash();
        ledger->setClosed();
        expect(ledger->assertSane());
        return ledger;
    }

    // Create an account represented by public RippleAddress and private
    // RippleAddress
    TestAccount
    createAccount()
    {
        static RippleAddress const seed
                = RippleAddress::createSeedGeneric ("masterpassphrase");
        static RippleAddress const generator
                = RippleAddress::createGeneratorPublic (seed);
        static int iSeq = -1;
        ++iSeq;
        return std::make_pair(RippleAddress::createAccountPublic(generator, iSeq),
                              std::uint64_t(0));
    }

    void
    freezeAccount(TestAccount& account, Ledger::pointer const& ledger)
    {
        Json::Value tx_json;
        tx_json["TransactionType"] = "AccountSet";
        tx_json["Fee"] = std::to_string(1000);
        tx_json["Account"] = account.first.humanAccountID();
        tx_json["SetFlag"] = asfGlobalFreeze;
        tx_json["Sequence"] = ++account.second;
        STTx tx = parseTransaction(account, tx_json);
        applyTransaction(ledger, tx);
    }

    void
    unfreezeAccount(TestAccount& account, Ledger::pointer const& ledger)
    {
        Json::Value tx_json;
        tx_json["TransactionType"] = "AccountSet";
        tx_json["Fee"] = std::to_string(1000);
        tx_json["Account"] = account.first.humanAccountID();
        tx_json["ClearFlag"] = asfGlobalFreeze;
        tx_json["Sequence"] = ++account.second;
        STTx tx = parseTransaction(account, tx_json);
        applyTransaction(ledger, tx);
    }

    void
    makePayment(TestAccount& from, TestAccount const& to,
                std::uint64_t amountDrops,
                std::uint64_t feeDrops,
                Ledger::pointer const& ledger)
    {
        Json::Value tx_json;
        tx_json["Account"] = from.first.humanAccountID();
        tx_json["Amount"] = std::to_string(amountDrops);
        tx_json["Destination"] = to.first.humanAccountID();
        tx_json["TransactionType"] = "Payment";
        tx_json["Fee"] = std::to_string(feeDrops);
        tx_json["Sequence"] = ++from.second;
        tx_json["Flags"] = tfUniversal;
        STTx tx = parseTransaction(from, tx_json);
        applyTransaction(ledger, tx);
    }

    void
    makePaymentVBC(TestAccount& from, TestAccount const& to,
                   std::uint64_t amountDrops,
                   std::uint64_t feeDrops,
                   Ledger::pointer const& ledger)
    {
        Json::Value tx_json;
        tx_json["Account"] = from.first.humanAccountID();
        Json::Value & amount = tx_json["Amount"];
        amount["value"] = std::to_string(amountDrops);
        amount["currency"] = "VBC";
        tx_json["Destination"] = to.first.humanAccountID();
        tx_json["TransactionType"] = "Payment";
        tx_json["Fee"] = std::to_string(feeDrops);
        tx_json["Sequence"] = ++from.second;
        tx_json["Flags"] = tfUniversal;
        STTx tx = parseTransaction(from, tx_json);
        applyTransaction(ledger, tx);
    }

    void
    makePayment(TestAccount& from, TestAccount const& to,
                Amount const amount,
                Ledger::pointer const& ledger)
    {
        Json::Value tx_json;
        tx_json["Account"] = from.first.humanAccountID();
        tx_json["Amount"] = amount.getJson();
        tx_json["Destination"] = to.first.humanAccountID();
        tx_json["TransactionType"] = "Payment";
        tx_json["Fee"] = std::to_string(1000);
        tx_json["Sequence"] = ++from.second;
        tx_json["Flags"] = tfUniversal;
        STTx tx = parseTransaction(from, tx_json);
        applyTransaction(ledger, tx);
    }

    void
    makePayment(TestAccount& from, TestAccount const& to,
                std::string const& currency, std::string const& amount,
                Ledger::pointer const& ledger)
    {
        makePayment(from, to, Amount(std::stod(amount), currency, to), ledger);
    }

    void
    makeIssue(TestAccount& from, TestAccount const& to,
              std::string const& amount,
              Ledger::pointer const& ledger)
    {
        Json::Value tx_json;
        tx_json["Account"] = from.first.humanAccountID();
        tx_json["Amount"] = Amount(std::stod(amount), to_string(assetCurrency()), from).getJson();
        tx_json["Destination"] = to.first.humanAccountID();
        Json::Value& releaseSchedule = tx_json["ReleaseSchedule"];
        Json::Value releasePoint;
        Json::Value& releaseRate = releasePoint["ReleasePoint"];
        releaseRate["Expiration"] = 0;
        releaseRate["ReleaseRate"] = 100000000;
        releaseSchedule.append(releasePoint);
        releaseRate["Expiration"] = 86400;
        releaseRate["ReleaseRate"] = 900000000;
        releaseSchedule.append(releasePoint);
        tx_json["TransactionType"] = "Issue";
        tx_json["Fee"] = std::to_string(1000);
        tx_json["Sequence"] = ++from.second;
        tx_json["Flags"] = tfUniversal;
        STTx tx = parseTransaction(from, tx_json);
        applyTransaction(ledger, tx);
    }

    void
    createOffer(TestAccount& from, Amount const& in, Amount const& out,
                Ledger::pointer ledger)
    {
        Json::Value tx_json;
        tx_json["TransactionType"] = "OfferCreate";
        tx_json["Fee"] = std::to_string(1000);
        tx_json["Account"] = from.first.humanAccountID();
        tx_json["TakerPays"] = in.getJson();
        tx_json["TakerGets"] = out.getJson();
        tx_json["Sequence"] = ++from.second;
        STTx tx = parseTransaction(from, tx_json);
        applyTransaction(ledger, tx);
    }

    // As currently implemented, this will cancel only the last offer made
    // from this account.
    void
    cancelOffer(TestAccount& from, Ledger::pointer ledger)
    {
        Json::Value tx_json;
        tx_json["TransactionType"] = "OfferCancel";
        tx_json["Fee"] = std::to_string(1000);
        tx_json["Account"] = from.first.humanAccountID();
        tx_json["OfferSequence"] = from.second;
        tx_json["Sequence"] = ++from.second;
        STTx tx = parseTransaction(from, tx_json);
        applyTransaction(ledger, tx);
    }

    void
    makeTrustSet(TestAccount& from, TestAccount const& issuer,
                 std::string const& currency, double amount,
                 Ledger::pointer const& ledger, uint32_t flags = tfClearNoRipple)
    {
        Json::Value tx_json;
        tx_json["Account"] = from.first.humanAccountID();
        Json::Value& limitAmount = tx_json["LimitAmount"];
        limitAmount["currency"] = currency;
        limitAmount["issuer"] = issuer.first.humanAccountID();
        limitAmount["value"] = std::to_string(amount);
        tx_json["TransactionType"] = "TrustSet";
        tx_json["Fee"] = std::to_string(1000);
        tx_json["Sequence"] = ++from.second;
        tx_json["Flags"] = flags;
        STTx tx = parseTransaction(from, tx_json);
        applyTransaction(ledger, tx);
    }

    Ledger::pointer
    close_and_advance(Ledger::pointer ledger, Ledger::pointer LCL)
    {
        SHAMap::pointer set = ledger->peekTransactionMap();
        CanonicalTXSet retriableTransactions(set->getHash());
        Ledger::pointer newLCL = std::make_shared<Ledger>(false, *LCL);
        // Set up to write SHAMap changes to our database,
        //   perform updates, extract changes
        applyTransactions(set, newLCL, newLCL, retriableTransactions, false);
        newLCL->updateSkipList();
        newLCL->setClosed();
        newLCL->peekAccountStateMap()->flushDirty(
            hotACCOUNT_NODE, newLCL->getLedgerSeq());
        newLCL->peekTransactionMap()->flushDirty(
            hotTRANSACTION_NODE, newLCL->getLedgerSeq());
        using namespace std::chrono;
        auto const epoch_offset = days(10957);  // 2000-01-01
        std::uint32_t closeTime = time_point_cast<seconds>  // now
                                         (system_clock::now()-epoch_offset).
                                         time_since_epoch().count();
        int CloseResolution = seconds(LEDGER_TIME_ACCURACY).count();
        bool closeTimeCorrect = true;
        newLCL->setAccepted(closeTime, CloseResolution, closeTimeCorrect);
        return newLCL;
    }

    void test_genesisLedger ()
    {
        std::uint64_t const xrp = std::mega::num;

        // Create master account
        auto master = createAccount();

        // Create genesis ledger
        Ledger::pointer LCL = createGenesisLedger(100000*xrp, master);

        // Create open scratch ledger
        Ledger::pointer ledger = std::make_shared<Ledger>(false, *LCL);

        // Create user accounts
        auto gw1 = createAccount();
        auto gw2 = createAccount();
        auto gw3 = createAccount();
        auto alice = createAccount();
        auto mark = createAccount();

        // Fund gw1, gw2, gw3, alice, mark from master
        makePayment(master, gw1, 5000*xrp, (0.01+50)*xrp, ledger);
        makePayment(master, gw2, 4000*xrp, (0.01+40)*xrp, ledger);
        makePayment(master, gw3, 3000*xrp, (0.01+30)*xrp, ledger);
        makePayment(master, alice, 2000*xrp, (0.01+20)*xrp, ledger);
        
        makePaymentVBC(master, gw1, 5000*xrp, (50)*xrp, ledger);
        makePaymentVBC(master, gw2, 4000*xrp, (40)*xrp, ledger);
        makePaymentVBC(master, gw3, 3000*xrp, (30)*xrp, ledger);
        makePaymentVBC(master, alice, 2000*xrp, (20)*xrp, ledger);
        
        makePaymentVBC(master, mark, 1000*xrp, (0.01+10)*xrp, ledger);
        makePayment(master, mark, 1000*xrp, (10)*xrp, ledger);

        LCL = close_and_advance(ledger, LCL);
        ledger = std::make_shared<Ledger>(false, *LCL);

        // alice trusts FOO/gw1
        makeTrustSet(alice, gw1, "FOO", 1, ledger);

        // mark trusts FOO/gw2
        makeTrustSet(mark, gw2, "FOO", 1, ledger);

        // mark trusts FOO/gw3
        makeTrustSet(mark, gw3, "FOO", 1, ledger);

        // gw2 pays mark with FOO
        makePayment(gw2, mark, "FOO", ".1", ledger);

        // gw3 pays mark with FOO
        makePayment(gw3, mark, "FOO", ".2", ledger);

        // gw1 pays alice with FOO
        makePayment(gw1, alice, "FOO", ".3", ledger);

        LCL = close_and_advance(ledger, LCL);
        ledger = std::make_shared<Ledger>(false, *LCL);

        createOffer(mark, Amount(1, "FOO", gw1), Amount(1, "FOO", gw2), ledger);
        createOffer(mark, Amount(1, "FOO", gw2), Amount(1, "FOO", gw3), ledger);
        cancelOffer(mark, ledger);
        freezeAccount(alice, ledger);

        LCL = close_and_advance(ledger, LCL);
        ledger = std::make_shared<Ledger>(false, *LCL);

        makePayment(alice, mark, 1*xrp, (0.001)*xrp, ledger);

        LCL = close_and_advance(ledger, LCL);
        ledger = std::make_shared<Ledger>(false, *LCL);

        // gw1 issue ASSET
        makeIssue(gw1, mark, "1000", ledger);

        LCL = close_and_advance(ledger, LCL);
        ledger = std::make_shared<Ledger>(false, *LCL);

        // mark trusts ASSET/gw1
        makeTrustSet(gw2, gw1, to_string(assetCurrency()), 10, ledger, tfSetNoRipple);

        LCL = close_and_advance(ledger, LCL);
        ledger = std::make_shared<Ledger>(false, *LCL);

        // gw1 pays alice with FOO
        makePayment(mark, gw2, Amount(5, to_string(assetCurrency()), gw1), ledger);

        LCL = close_and_advance(ledger, LCL);
        ledger = std::make_shared<Ledger>(false, *LCL);
    }

    void test_getQuality ()
    {
        uint256 uBig (
            "D2DC44E5DC189318DB36EF87D2104CDF0A0FE3A4B698BEEE55038D7EA4C68000");
        expect (6125895493223874560 == getQuality (uBig));
    }
public:
    void run ()
    {
        test_genesisLedger ();
        test_getQuality ();
    }
};

BEAST_DEFINE_TESTSUITE(Ledger,ripple_app,ripple);

} // ripple
