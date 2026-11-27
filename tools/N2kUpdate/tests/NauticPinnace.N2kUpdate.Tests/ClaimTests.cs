using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Testing;

namespace NauticPinnace.N2kUpdate.Tests;

/// <summary>The sender's address claim (12.3): win, lose, contest, requests.</summary>
public static class ClaimTests
{
    private sealed class Rig
    {
        public Simulation Sim = new();
        public VirtualLink Peer;
        public N2kEngine Engine;
        public List<CanFrame> PeerRx = new();

        public Rig(int unique = 0x0ACE5)
        {
            var link = Sim.Bus.Attach("sender", 66);
            Peer = Sim.Bus.Attach("peer");
            Peer.Open();
            Peer.FrameReceived += (f, _) => PeerRx.Add(f);
            Engine = new N2kEngine(link, Sim.Clock, new EngineOptions { UniqueNumber = unique });
            Sim.Nodes.Add(new EngineNode(Engine));
        }

        public void Claim(int source, N2kName name) =>
            Peer.SendBurst([new CanFrame(CanId.Encode(6, N2kPgn.IsoAddressClaim, source, 255), name.ToBytes())]);

        public List<(int Src, N2kName Name)> ClaimsSeen() => PeerRx
            .Where(f => CanId.Decode(f.Id).Pgn == N2kPgn.IsoAddressClaim)
            .Select(f => (CanId.Decode(f.Id).Source, N2kName.Read(f.Data))).ToList();
    }

    [Test]
    public static void NameLayout()
    {
        var n = N2kName.ForUpdater(0x1ABCD);
        Assert.Equal(0x1ABCD, n.UniqueNumber, "unique number");
        Assert.Equal(2046, n.ManufacturerCode, "manufacturer 2046");
        Assert.Equal(130, n.DeviceFunction, "function 130 (PC gateway)");
        Assert.Equal(25, n.DeviceClass, "class 25");
        Assert.Equal(4, n.IndustryGroup, "industry group 4 (marine)");
        Assert.True(n.ArbitraryAddressCapable, "arbitrary address capable");
    }

    [Test]
    public static void WinsUncontested()
    {
        var r = new Rig();
        r.Engine.Start();
        r.Sim.RunFor(200_000);
        Assert.True(!r.Engine.Source.IsUsable, "not usable before 250 ms");
        r.Sim.RunFor(100_000);
        Assert.True(r.Engine.Source.IsUsable, "usable after 250 ms");
        Assert.Equal(230, r.Engine.Source.Address, "preferred address 230");
        Assert.Equal(1, r.ClaimsSeen().Count, "one claim on the bus");
    }

    [Test]
    public static void LosesToHigherPriorityName()
    {
        var r = new Rig();
        int changedTo = -1;
        r.Engine.Source.Changed += (_, n) => changedTo = n;
        r.Engine.Start();
        r.Sim.RunFor(300_000);
        // A node with a lower NAME (higher priority) claims 230.
        r.Claim(230, N2kName.Create(5, 135, 130, 25));
        r.Sim.RunFor(400_000);
        Assert.Equal(231, r.Engine.Source.Address, "moved to the next free address");
        Assert.Equal(231, changedTo, "changed event");
        Assert.True(r.ClaimsSeen().Any(c => c.Src == 231), "claimed 231 on the bus");
    }

    [Test]
    public static void DefendsAgainstLowerPriorityName()
    {
        var r = new Rig(unique: 5);
        r.Engine.Start();
        r.Sim.RunFor(300_000);
        int before = r.ClaimsSeen().Count;
        r.Claim(230, N2kName.Create(0x1FFFFF, 2046, 130, 25));   // higher NAME value: lower priority
        r.Sim.RunFor(100_000);
        Assert.Equal(230, r.Engine.Source.Address, "kept 230");
        Assert.Equal(before + 1, r.ClaimsSeen().Count, "defended with a new claim");
    }

    [Test]
    public static void SkipsAddressesInUse()
    {
        var r = new Rig();
        r.Engine.Link.Open();
        // 230 and 231 are taken before we start.
        r.Claim(230, N2kName.Create(1, 135, 130, 25));
        r.Claim(231, N2kName.Create(2, 135, 130, 25));
        r.Sim.RunFor(10_000);
        r.Engine.Start();
        r.Sim.RunFor(300_000);
        Assert.Equal(232, r.Engine.Source.Address, "first free address after the preferred one");
    }

    [Test]
    public static void AnswersRequests()
    {
        var r = new Rig();
        r.Engine.Start();
        r.Sim.RunFor(300_000);
        r.PeerRx.Clear();
        r.Peer.SendBurst([new CanFrame(CanId.Encode(6, N2kPgn.IsoRequest, 0x10, 255), [0x00, 0xEE, 0x00])]);
        r.Peer.SendBurst([new CanFrame(CanId.Encode(6, N2kPgn.IsoRequest, 0x10, 230), [0x14, 0xF0, 0x01])]);
        r.Sim.RunFor(100_000);
        Assert.Equal(1, r.ClaimsSeen().Count, "address claim answered");
        var asm = new FastPacketAssembler();
        ProductInfo? pi = null;
        foreach (var f in r.PeerRx.Where(f => CanId.Decode(f.Id).Pgn == N2kPgn.ProductInfo))
        {
            var p = asm.Add(230, 255, N2kPgn.ProductInfo, f.Data, 0);
            if (p is not null) pi = ProductInfo.Decode(p);
        }
        Assert.Equal("NauticPinnace Updater", pi?.ModelId, "product information model");
    }
}
