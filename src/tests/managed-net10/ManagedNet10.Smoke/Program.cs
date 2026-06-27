using System.Reflection;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Threading;

namespace ManagedNet10.Smoke;

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Method)]
internal sealed class SmokeAttribute(string name) : Attribute
{
    public string Name { get; } = name;
}

[Smoke("payload")]
internal sealed record class Payload<T>(T Value)
{
    private readonly string _secret = "leanclr";

    public string Secret => _secret;
}

internal readonly record struct Pair(int Left, int Right);

[InlineArray(16)]
internal struct InlineIntBuffer
{
    private int _element0;
}

internal struct InlineArrayHolder
{
    public int Length;
    public InlineIntBuffer Buffer;
    public int[]? Large;
}

internal static class Program
{
    private static Type? s_seenType;

    private static async Task Main()
    {
        TestBasics();
        TestGenericsDelegatesAndExceptions();
        TestReflection();
        TestSpan();
        TestThreadingSubset();
        await TestAsync();
    }

    private static void TestBasics()
    {
        TestPairArithmetic();
        TestBoxingMetadata();
    }

    private static void TestPairArithmetic()
    {
        var pair = new Pair(19, 23);
        Require(pair.Left + pair.Right == 42, "record struct arithmetic failed");
    }

    private static void TestBoxingMetadata()
    {
        var pair = new Pair(19, 23);
        object boxed = pair;
        Require(boxed.GetType().Name == nameof(Pair), "boxing metadata failed");
    }

    private static void TestBoxOnly()
    {
        var pair = new Pair(19, 23);
        object boxed = pair;
        Require(boxed != null, "box object failed");
    }

    private static void TestBoxGetTypeOnly()
    {
        var pair = new Pair(19, 23);
        object boxed = pair;
        Require(boxed.GetType() != null, "boxed GetType failed");
    }

    private static void TestBoxGetTypeNoCompare()
    {
        var pair = new Pair(19, 23);
        object boxed = pair;
        boxed.GetType();
    }

    private static void TestTypeOfNoCompare()
    {
        _ = typeof(Pair);
    }

    private static void TestTypeOfKeepAlive()
    {
        s_seenType = typeof(Pair);
        Require(s_seenType != null, "typeof keepalive failed");
    }

    private static void TestTypeOfNameOnly()
    {
        Require(typeof(Pair).Name == nameof(Pair), "typeof metadata name failed");
    }

    private static void TestBoxGetTypeNameOnly()
    {
        var pair = new Pair(19, 23);
        object boxed = pair;
        Require(boxed.GetType().Name == nameof(Pair), "boxed GetType name failed");
    }

    private static void TestGenericsDelegatesAndExceptions()
    {
        var payload = new Payload<int>(21);
        Func<int, int> doubleValue = static value => value * 2;
        Require(doubleValue(payload.Value) == 42, "generic delegate call failed");

        try
        {
            ThrowNested();
            throw new InvalidOperationException("unreachable");
        }
        catch (TargetInvocationException ex) when (ex.InnerException is NotSupportedException)
        {
            Require(ex.InnerException.Message.Contains("smoke", StringComparison.Ordinal), "exception filter failed");
        }
    }

    private static void TestPayloadCtorOnly()
    {
        var payload = new Payload<int>(21);
        Require(payload.Value == 21, "generic payload ctor failed");
    }

    private static void TestStaticDelegateCreateOnly()
    {
        Func<int, int> doubleValue = static value => value * 2;
        Require(doubleValue != null, "static delegate create failed");
    }

    private static void TestStaticDelegateInvokeOnly()
    {
        Func<int, int> doubleValue = static value => value * 2;
        Require(doubleValue(21) == 42, "static delegate invoke failed");
    }

    private static void TestExceptionCtorOnly()
    {
        var ex = new NotSupportedException("smoke");
        Require(ex.Message.Contains("smoke", StringComparison.Ordinal), "exception ctor failed");
    }

    private static void TestExceptionNewOnly()
    {
        var ex = new NotSupportedException();
        Require(ex != null, "exception new failed");
    }

    private static void TestExceptionMessageNewOnly()
    {
        var ex = new NotSupportedException("smoke");
        Require(ex != null, "exception message new failed");
    }

    private static void TestExceptionMessageReadOnly()
    {
        var ex = new NotSupportedException("smoke");
        Require(ex.Message != null, "exception message read failed");
    }

    private static void TestStringContainsOrdinalOnly()
    {
        Require("smoke".Contains("sm", StringComparison.Ordinal), "string contains ordinal failed");
    }

    private static void TestThrowCatchOnly()
    {
        try
        {
            throw new NotSupportedException("smoke");
        }
        catch (NotSupportedException ex)
        {
            Require(ex.Message.Contains("smoke", StringComparison.Ordinal), "exception catch failed");
        }
    }

    private static void TestNestedThrowCatchOnly()
    {
        try
        {
            ThrowNested();
        }
        catch (TargetInvocationException ex)
        {
            Require(ex.InnerException is NotSupportedException, "nested exception catch failed");
        }
    }

    private static void TestTargetInvocationCtorOnly()
    {
        var inner = new NotSupportedException("smoke");
        var ex = new TargetInvocationException(inner);
        Require(ex != null, "target invocation ctor failed");
    }

    private static void TestTargetInvocationMessageCtorOnly()
    {
        var inner = new NotSupportedException("smoke");
        var ex = new TargetInvocationException("wrapped", inner);
        Require(ex != null, "target invocation message ctor failed");
    }

    private static void TestTargetInvocationInnerReadOnly()
    {
        var inner = new NotSupportedException("smoke");
        var ex = new TargetInvocationException(inner);
        Require(ex.InnerException is NotSupportedException, "target invocation inner read failed");
    }

    private static void TestThrowTargetInvocationCatchOnly()
    {
        try
        {
            throw new TargetInvocationException(new NotSupportedException("smoke"));
        }
        catch (TargetInvocationException ex)
        {
            Require(ex.InnerException is NotSupportedException, "target invocation throw catch failed");
        }
    }

    private static void TestExceptionFilterOnly()
    {
        try
        {
            ThrowNested();
            throw new InvalidOperationException("unreachable");
        }
        catch (TargetInvocationException ex) when (ex.InnerException is NotSupportedException)
        {
            Require(ex.InnerException.Message.Contains("smoke", StringComparison.Ordinal), "exception filter failed");
        }
    }

    private static void TestReflection()
    {
        var type = typeof(Payload<string>);
        var attr = type.GetCustomAttribute<SmokeAttribute>();
        Require(attr?.Name == "payload", "custom attribute lookup failed");

        var field = type.GetField("_secret", BindingFlags.Instance | BindingFlags.NonPublic);
        Require(field != null, "private field lookup failed");

        var payload = new Payload<string>("value");
        Require((string?)field.GetValue(payload) == "leanclr", "private field read failed");
    }

    private static void TestReflectionTypeOnly()
    {
        var type = typeof(Payload<string>);
        Require(type != null, "reflection typeof failed");
    }

    private static void TestReflectionAttributeOnly()
    {
        var type = typeof(Payload<string>);
        var attr = type.GetCustomAttribute<SmokeAttribute>();
        Require(attr != null, "custom attribute object failed");
    }

    private static void TestReflectionAttributeNameOnly()
    {
        var type = typeof(Payload<string>);
        var attr = type.GetCustomAttribute<SmokeAttribute>();
        Require(attr?.Name == "payload", "custom attribute name failed");
    }

    private static void TestAttributeCtorOnly()
    {
        var attr = new SmokeAttribute("payload");
        Require(attr.Name == "payload", "custom attribute ctor failed");
    }

    private static void TestAttributeSubclassOnly()
    {
        Require(typeof(SmokeAttribute).IsSubclassOf(typeof(Attribute)), "attribute subclass check failed");
    }

    private static void TestTypeEqualityOnly()
    {
        Require(typeof(SmokeAttribute) == typeof(SmokeAttribute), "type equality failed");
    }

    private static void TestAttributeBaseTypeOnly()
    {
        Require(typeof(SmokeAttribute).BaseType != null, "attribute base type lookup failed");
    }

    private static void TestAttributeBaseTypeCompareOnly()
    {
        Require(typeof(SmokeAttribute).BaseType == typeof(Attribute), "attribute base type compare failed");
    }

    private static void TestReflectionAttributeArrayOnly()
    {
        var type = typeof(Payload<string>);
        var attrs = type.GetCustomAttributes(typeof(SmokeAttribute), inherit: true);
        Require(attrs.Length == 1, "custom attribute array lookup failed");
    }

    private static void TestReflectionFieldLookupOnly()
    {
        var type = typeof(Payload<string>);
        var field = type.GetField("_secret", BindingFlags.Instance | BindingFlags.NonPublic);
        Require(field != null, "private field lookup failed");
    }

    private static void TestReflectionFieldValueOnly()
    {
        var type = typeof(Payload<string>);
        var field = type.GetField("_secret", BindingFlags.Instance | BindingFlags.NonPublic);
        var payload = new Payload<string>("value");
        Require((string?)field?.GetValue(payload) == "leanclr", "private field read failed");
    }

    private static void TestSpan()
    {
        TestSpanStackalloc();
        TestSpanStackallocInitializer();
        TestSpanBoolStackalloc();
        TestInlineArrayStructLayout();
    }

    private static void TestSpanStackalloc()
    {
        Span<int> values = stackalloc int[6];
        values[0] = 42;
        Require(values[0] == 42, "span stackalloc failed");
    }

    private static void TestSpanStackallocInitializer()
    {
        Span<int> values = stackalloc[] { 4, 8, 15, 16, 23, 42 };
        values[0] = values[^1];
        Require(values[0] == 42, "span index-from-end failed");
    }

    private static void TestSpanBoolStackalloc()
    {
        Span<bool> values = stackalloc bool[2];
        values[1] = true;
        Require(values[1], "span bool stackalloc failed");
        Require(ReadSpanBoolArg(values), "span bool argument read failed");
    }

    private static bool ReadSpanBoolArg(Span<bool> values)
    {
        return values[1];
    }

    private static void TestInlineArrayStructLayout()
    {
        var holder = new InlineArrayHolder();
        holder.Length = 16;
        holder.Buffer[15] = 42;
        Require(holder.Large == null, "inline array tail reference overlapped");

        holder.Large = [7, 42];
        Require(holder.Buffer[15] == 42, "inline array element was clobbered");
        Require(holder.Large[1] == 42, "inline array tail reference read failed");
    }

    private static void TestThreadingSubset()
    {
        var value = 0;
        Require(Interlocked.Increment(ref value) == 1, "interlocked increment failed");
        Volatile.Write(ref value, 41);
        Require(Volatile.Read(ref value) == 41, "volatile read/write failed");
    }

    private static void TestCurrentThreadOnly()
    {
        Require(Thread.CurrentThread != null, "current thread lookup failed");
    }

    private static void TestTaskFromResultOnly()
    {
        var task = Task.FromResult(42);
        Require(task != null, "Task.FromResult returned null");
    }

    private static void TestTaskFromResultValueOnly()
    {
        Require(Task.FromResult(42).Result == 42, "Task.FromResult value failed");
    }

    private static void TestReturnsResultAsyncOnly()
    {
        Require(ReturnsResultAsync().Result == 42, "ReturnsResultAsync value failed");
    }

    private static void TestYieldAwaiterOnly()
    {
        var awaiter = Task.Yield().GetAwaiter();
        awaiter.GetResult();
    }

    private static void TestAsyncTaskMethodBuilderTaskOnly()
    {
        var builder = AsyncTaskMethodBuilder.Create();
        Require(builder.Task != null, "async builder task failed");
    }

    private static void TestAsyncTaskMethodBuilderSetResultOnly()
    {
        var builder = AsyncTaskMethodBuilder.Create();
        builder.SetResult();
        Require(builder.Task != null, "async builder set result failed");
    }

    private static void TestAsyncNoAwaitCreateOnly()
    {
        var task = AsyncNoAwait();
        Require(task != null, "async no-await method returned null");
    }

    private static void TestAsyncCompletedAwaitCreateOnly()
    {
        var task = AsyncCompletedAwait();
        Require(task != null, "async completed-await method returned null");
    }

    private static void TestAsyncCreateOnly()
    {
        var task = TestAsync();
        Require(task != null, "async method returned null");
    }

    private static async Task AsyncNoAwait()
    {
        Require(true, "async no-await body failed");
    }

    private static async Task AsyncCompletedAwait()
    {
        Require(await ReturnsResultAsync() == 42, "async completed-await result failed");
    }

    private static async Task TestAsync()
    {
        await Task.Yield();
        Require(await ReturnsResultAsync() == 42, "async result failed");
    }

    private static Task<int> ReturnsResultAsync()
    {
        return Task.FromResult(42);
    }

    private static void ThrowNested()
    {
        try
        {
            throw new NotSupportedException("smoke");
        }
        catch (NotSupportedException ex)
        {
            throw new TargetInvocationException(ex);
        }
    }

    private static void Require([DoesNotReturnIf(false)] bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
