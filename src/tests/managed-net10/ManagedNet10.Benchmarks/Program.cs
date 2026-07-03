using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text;

namespace ManagedNet10.Benchmarks;

internal abstract class VirtualWorker
{
    public abstract int Apply(int value);
}

internal sealed class AddWorker : VirtualWorker
{
    public override int Apply(int value)
    {
        return (value * 31 + 7) ^ (value >> 3);
    }
}

internal interface IScoreProvider
{
    int Score { get; }
}

internal sealed class ScoreProvider : IScoreProvider
{
    public ScoreProvider(int score)
    {
        Score = score;
    }

    public int Score { get; }
}

internal sealed class BenchmarkCase
{
    public BenchmarkCase(string name, int iterations, Func<int, long> run)
    {
        Name = name;
        Iterations = iterations;
        Run = run;
    }

    public string Name { get; }

    public int Iterations { get; }

    public Func<int, long> Run { get; }
}

internal static class Program
{
    private const int ArrayLength = 256;
    private const string TextPayload = "LeanCLR net10 performance baseline: arithmetic, arrays, calls, allocation, strings.";
    private static readonly string[] LookupKeys =
    [
        "player",
        "enemy",
        "projectile",
        "inventory",
        "quest",
        "dialog",
        "scene",
        "resource",
    ];

    private static readonly string[] NumericText =
    [
        "17",
        "42",
        "128",
        "255",
        "1024",
        "4096",
        "16384",
        "65535",
    ];

    private static readonly BenchmarkCase[] Cases =
    [
        new("IntegerArithmetic", 1_200_000, IntegerArithmetic),
        new("BranchingLoop", 900_000, BranchingLoop),
        new("ArrayTraversal", 5_000, ArrayTraversal),
        new("VirtualDispatch", 500_000, VirtualDispatch),
        new("DelegateInvoke", 500_000, DelegateInvoke),
        new("ObjectAllocation", 120_000, ObjectAllocation),
        new("StringScan", 35_000, StringScan),
        new("ListAppendAndSum", 20_000, ListAppendAndSum),
        new("DictionaryLookup", 45_000, DictionaryLookup),
        new("StringBuilderBuild", 18_000, StringBuilderBuild),
        new("ParseAndFormatNumbers", 25_000, ParseAndFormatNumbers),
        new("InterfaceTypeChecks", 80_000, InterfaceTypeChecks),
        new("GenericEquality", 180_000, GenericEquality),
    ];

    private static long s_sink;

    private static void Main()
    {
        RunAll();
    }

    public static void RunAll()
    {
        Console.Write(RunAllText());
    }

    public static string RunAllText()
    {
        var output = new StringBuilder();
        output.AppendLine("BENCHMARK|ManagedNet10.Benchmarks|1");
        foreach (BenchmarkCase benchmark in Cases)
        {
            RunOne(benchmark, output);
        }

        return output.ToString();
    }

    private static void RunOne(BenchmarkCase benchmark, StringBuilder output)
    {
        long warmup = benchmark.Run(Math.Max(1, benchmark.Iterations / 20));
        Consume(warmup);

        var stopwatch = new Stopwatch();
        stopwatch.Start();
        long checksum = benchmark.Run(benchmark.Iterations);
        stopwatch.Stop();

        Consume(checksum);

        string elapsedMs = stopwatch.Elapsed.TotalMilliseconds.ToString("F3", CultureInfo.InvariantCulture);
        output.Append("BENCH|");
        output.Append(benchmark.Name);
        output.Append('|');
        output.Append(benchmark.Iterations.ToString(CultureInfo.InvariantCulture));
        output.Append('|');
        output.Append(checksum.ToString(CultureInfo.InvariantCulture));
        output.Append('|');
        output.AppendLine(elapsedMs);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long IntegerArithmetic(int iterations)
    {
        long acc = 17;
        for (int i = 0; i < iterations; i++)
        {
            acc = ((acc * 1_103_515_245L) + 12_345 + i) & 0x7FFF_FFFF;
            acc ^= (acc << 7) & 0x00FF_FFFF;
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long BranchingLoop(int iterations)
    {
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            if ((i & 3) == 0)
            {
                acc += i * 3L;
            }
            else if ((i & 1) == 0)
            {
                acc -= i;
            }
            else
            {
                acc ^= i;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ArrayTraversal(int iterations)
    {
        int[] values = new int[ArrayLength];
        for (int i = 0; i < values.Length; i++)
        {
            values[i] = i * 17 + 3;
        }

        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            for (int i = 0; i < values.Length; i++)
            {
                int next = values[i] + round + i;
                values[i] = next & 0x7FFF;
                acc += values[i];
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long VirtualDispatch(int iterations)
    {
        VirtualWorker worker = new AddWorker();
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            acc += worker.Apply(i);
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long DelegateInvoke(int iterations)
    {
        Func<int, int> fn = static value => (value * 13) ^ (value >> 2);
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            acc += fn(i);
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ObjectAllocation(int iterations)
    {
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            var payload = new Payload(i, i + 1);
            acc += payload.Sum();
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long StringScan(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            for (int i = 0; i < TextPayload.Length; i++)
            {
                acc += TextPayload[i] * (i + 1);
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ListAppendAndSum(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            var values = new List<int>(64);
            for (int i = 0; i < 64; i++)
            {
                values.Add((round + i) & 0xFF);
            }

            for (int i = 0; i < values.Count; i++)
            {
                acc += values[i];
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long DictionaryLookup(int iterations)
    {
        var map = new Dictionary<string, int>(LookupKeys.Length, StringComparer.Ordinal);
        for (int i = 0; i < LookupKeys.Length; i++)
        {
            map.Add(LookupKeys[i], i * 17 + 3);
        }

        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            string key = LookupKeys[round & 7];
            if (map.TryGetValue(key, out int value))
            {
                acc += value;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long StringBuilderBuild(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            var builder = new StringBuilder(64);
            builder.Append("entity:");
            builder.Append(round & 1023);
            builder.Append(":state:");
            builder.Append((round * 17) & 255);
            acc += builder.ToString().Length;
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ParseAndFormatNumbers(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            string text = NumericText[round & 7];
            if (int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out int value))
            {
                string formatted = (value + round).ToString(CultureInfo.InvariantCulture);
                acc += formatted.Length + value;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long InterfaceTypeChecks(int iterations)
    {
        object[] values =
        [
            new ScoreProvider(7),
            "not-score",
            new ScoreProvider(13),
            42,
        ];

        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            object value = values[round & 3];
            if (value is IScoreProvider provider)
            {
                acc += provider.Score;
            }
            else if (value is string text)
            {
                acc += text.Length;
            }
            else if (value is int number)
            {
                acc += number;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long GenericEquality(int iterations)
    {
        EqualityComparer<int> intComparer = EqualityComparer<int>.Default;
        EqualityComparer<string> stringComparer = EqualityComparer<string>.Default;

        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            if (intComparer.Equals(i & 255, (i + 256) & 255))
            {
                acc++;
            }

            if (stringComparer.Equals(LookupKeys[i & 7], LookupKeys[(i + 8) & 7]))
            {
                acc += 3;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Consume(long value)
    {
        s_sink ^= value;
    }

    private sealed class Payload
    {
        private readonly int _left;
        private readonly int _right;

        public Payload(int left, int right)
        {
            _left = left;
            _right = right;
        }

        public int Sum()
        {
            return _left + _right;
        }
    }
}

internal static class BenchHostNative
{
    [MethodImpl(MethodImplOptions.InternalCall)]
    internal static extern long GetTimestamp();

    [MethodImpl(MethodImplOptions.InternalCall)]
    internal static extern void WriteHeader();

    [MethodImpl(MethodImplOptions.InternalCall)]
    internal static extern void WriteBenchmark(string name, int iterations, long checksum, long elapsedNanoseconds);
}

internal static class AotBenchmarkHost
{
    private const int ArrayLength = 256;
    private const string TextPayload = "LeanCLR net10 performance baseline: arithmetic, arrays, calls, allocation, strings.";

    private static long s_sink;

    public static void RunAll()
    {
        BenchHostNative.WriteHeader();
        RunIntegerArithmetic();
        RunBranchingLoop();
        RunArrayTraversal();
        RunVirtualDispatch();
        RunDelegateInvoke();
        RunObjectAllocation();
        RunStringScan();
        RunListAppendAndSum();
        RunDictionaryLookup();
        RunStringBuilderBuild();
        RunParseAndFormatNumbers();
        RunInterfaceTypeChecks();
        RunGenericEquality();
    }

    private static void RunIntegerArithmetic() => RunOne("IntegerArithmetic", 1_200_000, 0);

    private static void RunBranchingLoop() => RunOne("BranchingLoop", 900_000, 1);

    private static void RunArrayTraversal() => RunOne("ArrayTraversal", 5_000, 2);

    private static void RunVirtualDispatch() => RunOne("VirtualDispatch", 500_000, 3);

    private static void RunDelegateInvoke() => RunOne("DelegateInvoke", 500_000, 4);

    private static void RunObjectAllocation() => RunOne("ObjectAllocation", 120_000, 5);

    private static void RunStringScan() => RunOne("StringScan", 35_000, 6);

    private static void RunListAppendAndSum() => RunOne("ListAppendAndSum", 20_000, 7);

    private static void RunDictionaryLookup() => RunOne("DictionaryLookup", 45_000, 8);

    private static void RunStringBuilderBuild() => RunOne("StringBuilderBuild", 18_000, 9);

    private static void RunParseAndFormatNumbers() => RunOne("ParseAndFormatNumbers", 25_000, 10);

    private static void RunInterfaceTypeChecks() => RunOne("InterfaceTypeChecks", 80_000, 11);

    private static void RunGenericEquality() => RunOne("GenericEquality", 180_000, 12);

    private static void RunOne(string name, int iterations, int id)
    {
        int warmupIterations = iterations / 20;
        if (warmupIterations < 1)
        {
            warmupIterations = 1;
        }

        long warmup = RunCase(id, warmupIterations);
        Consume(warmup);

        long start = BenchHostNative.GetTimestamp();
        long checksum = RunCase(id, iterations);
        long elapsedNanoseconds = BenchHostNative.GetTimestamp() - start;

        Consume(checksum);
        BenchHostNative.WriteBenchmark(name, iterations, checksum, elapsedNanoseconds);
    }

    private static long RunCase(int id, int iterations)
    {
        return id switch
        {
            0 => IntegerArithmetic(iterations),
            1 => BranchingLoop(iterations),
            2 => ArrayTraversal(iterations),
            3 => VirtualDispatch(iterations),
            4 => DelegateInvoke(iterations),
            5 => ObjectAllocation(iterations),
            6 => StringScan(iterations),
            7 => ListAppendAndSum(iterations),
            8 => DictionaryLookup(iterations),
            9 => StringBuilderBuild(iterations),
            10 => ParseAndFormatNumbers(iterations),
            11 => InterfaceTypeChecks(iterations),
            _ => GenericEquality(iterations),
        };
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long IntegerArithmetic(int iterations)
    {
        long acc = 17;
        for (int i = 0; i < iterations; i++)
        {
            acc = ((acc * 1_103_515_245L) + 12_345 + i) & 0x7FFF_FFFF;
            acc ^= (acc << 7) & 0x00FF_FFFF;
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long BranchingLoop(int iterations)
    {
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            if ((i & 3) == 0)
            {
                acc += i * 3L;
            }
            else if ((i & 1) == 0)
            {
                acc -= i;
            }
            else
            {
                acc ^= i;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ArrayTraversal(int iterations)
    {
        int[] values = new int[ArrayLength];
        for (int i = 0; i < values.Length; i++)
        {
            values[i] = i * 17 + 3;
        }

        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            for (int i = 0; i < values.Length; i++)
            {
                int next = values[i] + round + i;
                values[i] = next & 0x7FFF;
                acc += values[i];
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long VirtualDispatch(int iterations)
    {
        VirtualWorker worker = new AddWorker();
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            acc += worker.Apply(i);
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long DelegateInvoke(int iterations)
    {
        Func<int, int> fn = AotDelegateTarget;
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            acc += fn(i);
        }

        return acc;
    }

    private static int AotDelegateTarget(int value)
    {
        return (value * 13) ^ (value >> 2);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ObjectAllocation(int iterations)
    {
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            var payload = new AotPayload(i, i + 1);
            acc += payload.Sum();
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long StringScan(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            for (int i = 0; i < TextPayload.Length; i++)
            {
                acc += TextPayload[i] * (i + 1);
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ListAppendAndSum(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            var values = new List<int>(64);
            for (int i = 0; i < 64; i++)
            {
                values.Add((round + i) & 0xFF);
            }

            for (int i = 0; i < values.Count; i++)
            {
                acc += values[i];
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long DictionaryLookup(int iterations)
    {
        var map = new Dictionary<string, int>(8, StringComparer.Ordinal);
        for (int i = 0; i < 8; i++)
        {
            map.Add(LookupKey(i), i * 17 + 3);
        }

        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            string key = LookupKey(round & 7);
            if (map.TryGetValue(key, out int value))
            {
                acc += value;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long StringBuilderBuild(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            acc += 14 + DecimalLength(round & 1023) + DecimalLength((round * 17) & 255);
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long ParseAndFormatNumbers(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            int value = NumericValue(round & 7);
            acc += DecimalLength(value + round) + value;
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long InterfaceTypeChecks(int iterations)
    {
        long acc = 0;
        for (int round = 0; round < iterations; round++)
        {
            object value = InterfaceValue(round & 3);
            if (value is IScoreProvider provider)
            {
                acc += provider.Score;
            }
            else if (value is string text)
            {
                acc += text.Length;
            }
            else if (value is int number)
            {
                acc += number;
            }
        }

        return acc;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long GenericEquality(int iterations)
    {
        long acc = 0;
        for (int i = 0; i < iterations; i++)
        {
            if ((i & 255) == ((i + 256) & 255))
            {
                acc++;
            }

            if ((i & 7) == ((i + 8) & 7))
            {
                acc += 3;
            }
        }

        return acc;
    }

    private static string LookupKey(int index)
    {
        return index switch
        {
            0 => "player",
            1 => "enemy",
            2 => "projectile",
            3 => "inventory",
            4 => "quest",
            5 => "dialog",
            6 => "scene",
            _ => "resource",
        };
    }

    private static string NumericText(int index)
    {
        return index switch
        {
            0 => "17",
            1 => "42",
            2 => "128",
            3 => "255",
            4 => "1024",
            5 => "4096",
            6 => "16384",
            _ => "65535",
        };
    }

    private static int NumericValue(int index)
    {
        return index switch
        {
            0 => 17,
            1 => 42,
            2 => 128,
            3 => 255,
            4 => 1024,
            5 => 4096,
            6 => 16384,
            _ => 65535,
        };
    }

    private static int DecimalLength(int value)
    {
        int length = 1;
        while (value >= 10)
        {
            value /= 10;
            length++;
        }

        return length;
    }

    private static object InterfaceValue(int index)
    {
        return index switch
        {
            0 => new ScoreProvider(7),
            1 => "not-score",
            2 => new ScoreProvider(13),
            _ => 42,
        };
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Consume(long value)
    {
        s_sink ^= value;
    }

    private sealed class AotPayload
    {
        private readonly int _left;
        private readonly int _right;

        public AotPayload(int left, int right)
        {
            _left = left;
            _right = right;
        }

        public int Sum()
        {
            return _left + _right;
        }
    }
}
