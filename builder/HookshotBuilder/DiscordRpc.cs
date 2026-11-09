using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace HookshotBuilder;

/// <summary>
/// Discord Rich Presence (Local IPC on discord-ipc-0..9) for Hookshot Builder and The Legend of Zelda: Ocarina of Time.
/// Runs on a background thread so UI actions never wait on Discord.
/// </summary>
static class DiscordRpc
{
    public const string DefaultClientId = "1442727160535056384";
    public const string HookshotIconUrl = "https://raw.githubusercontent.com/NotRexed/Hookshot/main/assets/hookshot.png";
    public const string OcarinaIconUrl = "https://upload.wikimedia.org/wikipedia/en/5/57/The_Legend_of_Zelda_Ocarina_of_Time.jpg";

    const uint OpHandshake = 0;
    const uint OpFrame = 1;
    const uint OpClose = 2;
    const uint OpPing = 3;
    const uint OpPong = 4;

    static readonly object Sync = new();
    static readonly ManualResetEventSlim Wake = new(false);
    static readonly long StartEpoch = DateTimeOffset.UtcNow.ToUnixTimeSeconds();

    static bool started;
    static bool stopping;
    static bool dirty;
    static bool playingOot;
    static int activePid = Environment.ProcessId;
    static long activityEpoch = StartEpoch;
    static string detailsText = "Configuring Game Files";
    static string stateText = "The Legend of Zelda: Ocarina of Time";
    static string lastStage = "";
    static DateTime lastProgressSent = DateTime.MinValue;
    static ulong nonce = 1;

    static bool Disabled
    {
        get
        {
            var off = Environment.GetEnvironmentVariable("HOOKSHOT_NO_DISCORD");
            return !string.IsNullOrEmpty(off) && off != "0";
        }
    }

    static string ClientId =>
        Environment.GetEnvironmentVariable("HOOKSHOT_DISCORD_BUILDER_APP_ID") is { Length: > 0 } b ? b :
        Environment.GetEnvironmentVariable("HOOKSHOT_DISCORD_APP_ID") is { Length: > 0 } a ? a :
        DefaultClientId;

    static void EnsureStarted()
    {
        if (started || Disabled) return;
        started = true;
        var thread = new Thread(WorkerLoop)
        {
            IsBackground = true,
            Name = "HookshotDiscordRpc",
            Priority = ThreadPriority.BelowNormal,
        };
        thread.Start();
    }

    public static void SetBuilderStatus(string details, string state = "The Legend of Zelda: Ocarina of Time")
    {
        if (Disabled) return;
        lock (Sync)
        {
            EnsureStarted();
            if (!playingOot && detailsText == details && stateText == state) return;
            playingOot = false;
            activePid = Environment.ProcessId;
            detailsText = details;
            stateText = state;
            dirty = true;
            Wake.Set();
        }
    }

    public static void SetBuilderProgress(string stageName, double fraction, bool updating, string? note = null)
    {
        if (Disabled) return;
        var now = DateTime.UtcNow;
        var pct = (int)Math.Round(Math.Clamp(fraction, 0, 1) * 100);
        var nextDetails = $"{(updating ? "Updating" : "Building")} Ocarina of Time ({pct}%)";
        var nextState = string.IsNullOrWhiteSpace(note) ? stageName : $"{stageName} · {note}";
        lock (Sync)
        {
            EnsureStarted();
            var stageChanged = !string.Equals(lastStage, stageName, StringComparison.Ordinal);
            if (!stageChanged && pct < 100 && (now - lastProgressSent).TotalSeconds < 2.0) return;
            lastStage = stageName;
            lastProgressSent = now;
            playingOot = false;
            activePid = Environment.ProcessId;
            detailsText = nextDetails;
            stateText = nextState;
            dirty = true;
            Wake.Set();
        }
    }

    public static void SetPlayingOcarinaOfTime(Process? gameProcess = null, string? version = null)
    {
        if (Disabled) return;
        lock (Sync)
        {
            EnsureStarted();
            playingOot = true;
            activePid = gameProcess is { HasExited: false } ? gameProcess.Id : Environment.ProcessId;
            activityEpoch = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            detailsText = "Playing The Legend of Zelda: Ocarina of Time";
            stateText = !string.IsNullOrWhiteSpace(version)
                ? $"Hookshot {version} · Native PC"
                : "Hookshot (Vulkan Native)";
            dirty = true;
            Wake.Set();
        }
        if (gameProcess != null)
        {
            Task.Run(() =>
            {
                try
                {
                    gameProcess.WaitForExit();
                }
                catch (Exception) { }
                SetBuilderStatus("Ready to Play", "The Legend of Zelda: Ocarina of Time");
            });
        }
    }

    public static void Shutdown()
    {
        lock (Sync)
        {
            if (!started) return;
            stopping = true;
            Wake.Set();
        }
    }

    public static string BuildActivityJson(
        bool isOcarinaOfTime,
        int pid,
        long startTimestamp,
        string details,
        string state,
        ulong requestNonce)
    {
        var hookshotImg = Environment.GetEnvironmentVariable("HOOKSHOT_DISCORD_ICON_URL") is { Length: > 0 } h ? h : HookshotIconUrl;
        var ootImg = Environment.GetEnvironmentVariable("HOOKSHOT_DISCORD_OOT_IMAGE") is { Length: > 0 } o ? o : OcarinaIconUrl;

        var payload = new
        {
            cmd = "SET_ACTIVITY",
            args = new
            {
                pid,
                activity = new
                {
                    name = isOcarinaOfTime ? "The Legend of Zelda: Ocarina of Time" : "Hookshot Builder",
                    type = 0,
                    details,
                    state,
                    timestamps = new { start = startTimestamp },
                    assets = new
                    {
                        large_image = isOcarinaOfTime ? ootImg : hookshotImg,
                        large_text = isOcarinaOfTime ? "The Legend of Zelda: Ocarina of Time" : "Hookshot Builder",
                        small_image = isOcarinaOfTime ? hookshotImg : ootImg,
                        small_text = isOcarinaOfTime ? "Hookshot (Vulkan Native)" : "The Legend of Zelda: Ocarina of Time",
                    },
                },
            },
            nonce = requestNonce.ToString(),
        };
        return JsonSerializer.Serialize(payload);
    }

    static byte[] PackFrame(uint opcode, string json)
    {
        var body = Encoding.UTF8.GetBytes(json);
        var frame = new byte[8 + body.Length];
        BitConverter.TryWriteBytes(frame.AsSpan(0, 4), opcode);
        BitConverter.TryWriteBytes(frame.AsSpan(4, 4), (uint)body.Length);
        Buffer.BlockCopy(body, 0, frame, 8, body.Length);
        return frame;
    }

    static NamedPipeClientStream? ConnectPipe()
    {
        var hsJson = JsonSerializer.Serialize(new { v = 1, client_id = ClientId });
        var hsFrame = PackFrame(OpHandshake, hsJson);
        for (var i = 0; i < 10; i++)
        {
            NamedPipeClientStream? pipe = null;
            try
            {
                pipe = new NamedPipeClientStream(".", $"discord-ipc-{i}", PipeDirection.InOut, PipeOptions.None);
                pipe.Connect(150);
                pipe.Write(hsFrame, 0, hsFrame.Length);
                pipe.Flush();
                var (op, reply) = ReadFrame(pipe);
                if (op == OpFrame && reply != null && reply.Contains("\"READY\"", StringComparison.Ordinal))
                    return pipe;
            }
            catch (Exception) { }
            try { pipe?.Dispose(); } catch (Exception) { }
        }
        return null;
    }

    static (uint Opcode, string? Json) ReadFrame(Stream stream)
    {
        Span<byte> hdr = stackalloc byte[8];
        var read = 0;
        while (read < 8)
        {
            var n = stream.Read(hdr[read..]);
            if (n <= 0) return (OpClose, null);
            read += n;
        }
        var op = BitConverter.ToUInt32(hdr[..4]);
        var len = BitConverter.ToInt32(hdr[4..8]);
        if (len <= 0 || len > (1 << 20)) return (op, "");
        var buf = new byte[len];
        var got = 0;
        while (got < len)
        {
            var n = stream.Read(buf, got, len - got);
            if (n <= 0) return (OpClose, null);
            got += n;
        }
        return (op, Encoding.UTF8.GetString(buf));
    }

    static void WorkerLoop()
    {
        NamedPipeClientStream? pipe = null;
        while (true)
        {
            Wake.Wait(pipe == null ? 5000 : 2000);
            Wake.Reset();

            bool isOot;
            int pid;
            long epoch;
            string details, state;
            ulong reqNonce;
            bool shouldSend;

            lock (Sync)
            {
                if (stopping) break;
                shouldSend = dirty || pipe == null;
                isOot = playingOot;
                pid = activePid;
                epoch = activityEpoch;
                details = detailsText;
                state = stateText;
                reqNonce = nonce++;
            }

            if (pipe is { IsConnected: false })
            {
                try { pipe.Dispose(); } catch (Exception) { }
                pipe = null;
            }

            if (pipe == null)
            {
                pipe = ConnectPipe();
                if (pipe == null) continue;
                shouldSend = true;
            }

            if (shouldSend)
            {
                try
                {
                    var json = BuildActivityJson(isOot, pid, epoch, details, state, reqNonce);
                    var frame = PackFrame(OpFrame, json);
                    pipe.Write(frame, 0, frame.Length);
                    pipe.Flush();
                    var (op, reply) = ReadFrame(pipe);
                    if (op == OpClose)
                    {
                        pipe.Dispose();
                        pipe = null;
                        continue;
                    }
                    if (op == OpPing && reply != null)
                    {
                        var pong = PackFrame(OpPong, reply);
                        pipe.Write(pong, 0, pong.Length);
                        pipe.Flush();
                    }
                    lock (Sync) dirty = false;
                }
                catch (Exception)
                {
                    try { pipe?.Dispose(); } catch (Exception) { }
                    pipe = null;
                }
            }
        }
        try { pipe?.Dispose(); } catch (Exception) { }
    }
}
