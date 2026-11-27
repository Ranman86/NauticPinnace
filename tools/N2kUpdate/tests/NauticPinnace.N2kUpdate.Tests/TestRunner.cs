using System.Diagnostics;
using System.Globalization;
using System.Reflection;

namespace NauticPinnace.N2kUpdate.Tests;

/// <summary>Marks a static test method. Slow tests only run with "realtime" on the command line.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class TestAttribute : Attribute
{
    public bool RealTime { get; init; }
}

public sealed class TestFailure(string message) : Exception(message);

/// <summary>A few assertions; failures carry enough context to read without a debugger.</summary>
public static class Assert
{
    public static void True(bool cond, string what)
    {
        if (!cond) throw new TestFailure(what);
    }

    public static void Equal<T>(T expected, T actual, string what)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new TestFailure(string.Create(CultureInfo.InvariantCulture, $"{what}: expected {expected}, got {actual}"));
    }

    public static void Hex(string expected, ReadOnlySpan<byte> actual, string what)
    {
        string e = expected.Replace(" ", "").Replace("\n", "").Replace("\r", "").ToUpperInvariant();
        string a = Convert.ToHexString(actual);
        if (e != a) throw new TestFailure($"{what}:\n  expected {Spaced(e)}\n  actual   {Spaced(a)}");
    }

    public static void Near(double expected, double actual, double tolerance, string what)
    {
        if (Math.Abs(expected - actual) > tolerance)
            throw new TestFailure(string.Create(CultureInfo.InvariantCulture, $"{what}: expected {expected} ± {tolerance}, got {actual}"));
    }

    private static string Spaced(string hex) =>
        string.Join(' ', Enumerable.Range(0, hex.Length / 2).Select(i => hex.Substring(2 * i, 2)));
}

public static class Program
{
    public static int Main(string[] args)
    {
        bool realtime = args.Contains("realtime", StringComparer.OrdinalIgnoreCase);
        var filters = args.Where(a => !a.Equals("realtime", StringComparison.OrdinalIgnoreCase)).ToArray();
        var tests = typeof(Program).Assembly.GetTypes()
            .SelectMany(t => t.GetMethods(BindingFlags.Public | BindingFlags.Static)
                .Select(m => (Type: t, Method: m, Attr: m.GetCustomAttribute<TestAttribute>())))
            .Where(x => x.Attr is not null)
            .Where(x => realtime || !x.Attr!.RealTime)
            .Where(x => filters.Length == 0 || filters.Any(f => $"{x.Type.Name}.{x.Method.Name}".Contains(f, StringComparison.OrdinalIgnoreCase)))
            .OrderBy(x => x.Type.Name).ThenBy(x => x.Method.MetadataToken)
            .ToList();

        int failed = 0;
        var total = Stopwatch.StartNew();
        foreach (var (type, method, _) in tests)
        {
            string name = $"{type.Name}.{method.Name}";
            var sw = Stopwatch.StartNew();
            try
            {
                method.Invoke(null, null);
                Console.WriteLine($"PASS  {name}  ({sw.ElapsedMilliseconds} ms)");
            }
            catch (TargetInvocationException tie)
            {
                failed++;
                var ex = tie.InnerException!;
                Console.WriteLine($"FAIL  {name}  ({sw.ElapsedMilliseconds} ms)");
                Console.WriteLine("      " + (ex is TestFailure ? ex.Message : ex.ToString()).Replace("\n", "\n      "));
            }
        }
        // Decimal point on every Windows. The tests themselves run under the
        // machine's culture on purpose: a culture-dependent bug in the library
        // shows up here instead of on a user's German Windows.
        Console.WriteLine(string.Create(CultureInfo.InvariantCulture, $"{tests.Count - failed} passed, {failed} failed, {total.Elapsed.TotalSeconds:F1} s"));
        return failed == 0 ? 0 : 1;
    }
}
