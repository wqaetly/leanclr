using System.Diagnostics.CodeAnalysis;

namespace ManagedNet10.Smoke;

public static class HostBridgeWrapperSmoke
{
    public static void Run()
    {
        var bridge = new MockHostBridge();
        var hostObject = HostObject.Create(bridge, "Mock.Node", "Player");
        Require(hostObject.Handle != 0, "host object wrapper did not receive a handle");
        Require(hostObject.IsAlive, "host object wrapper did not query liveness");

        var callbackCount = 0;
        using (var subscription = hostObject.Subscribe("OnClick", () => callbackCount++))
        {
            bridge.TriggerEvent(subscription.Token);
            Require(callbackCount == 0, "host event callback ran inline");
            HostDispatcher.PumpMainThread(bridge);
            Require(callbackCount == 1, "host event callback did not run through dispatcher");
        }

        bridge.TriggerLastEvent();
        HostDispatcher.PumpMainThread(bridge);
        Require(callbackCount == 1, "disposed event subscription should not enqueue callbacks");

        hostObject.Dispose();
        hostObject.Dispose();
        ExpectObjectDisposed(static state => ((HostObject)state!).EnsureAlive(), hostObject);

        var destroyedObject = HostObject.Create(bridge, "Mock.Node", "Destroyed");
        bridge.NotifyDestroyed(destroyedObject.Handle);
        ExpectObjectDisposed(static state => ((HostObject)state!).EnsureAlive(), destroyedObject);

        var failingObject = HostObject.Create(bridge, "Mock.Node", "FailingEvent");
        using (var failingSubscription = failingObject.Subscribe("OnFail", static () => throw new InvalidOperationException("managed callback failed")))
        {
            bridge.TriggerEvent(failingSubscription.Token);
            ExpectHostBridgeException(static state => HostDispatcher.PumpMainThread((MockHostBridge)state!),
                bridge,
                HostBridgeStatus.ManagedException);
        }

        bridge.FailNextCreate("host create failed");
        ExpectHostBridgeException(static state => HostObject.Create((MockHostBridge)state!, "Mock.Node", "Failure"),
            bridge,
            HostBridgeStatus.HostFailure);
    }

    private static void ExpectObjectDisposed(Action<object?> action, object? state)
    {
        try
        {
            action(state);
        }
        catch (ObjectDisposedException)
        {
            return;
        }

        throw new InvalidOperationException("expected ObjectDisposedException");
    }

    private static void ExpectHostBridgeException(Action<object?> action, object? state, HostBridgeStatus expectedStatus)
    {
        try
        {
            action(state);
        }
        catch (Exception ex)
        {
            if (ex is HostBridgeException hostBridgeException && hostBridgeException.Status == expectedStatus)
            {
                return;
            }
        }

        throw new InvalidOperationException("expected HostBridgeException");
    }

    private static void Require([DoesNotReturnIf(false)] bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

internal enum HostBridgeStatus
{
    Ok = 0,
    InvalidArgument = 1,
    UnsupportedVersion = 2,
    MissingCapability = 3,
    HostFailure = 4,
    ObjectDisposed = 5,
    ReentrantCall = 6,
    ManagedException = 7,
}

internal sealed class HostBridgeException : InvalidOperationException
{
    public HostBridgeException(HostBridgeStatus status, string message)
        : base(message)
    {
        Status = status;
    }

    public HostBridgeStatus Status { get; }
}

internal static class HostBridgeStatusConverter
{
    public static void ThrowIfFailed(HostBridgeStatus status, string? message, string objectName)
    {
        if (status == HostBridgeStatus.Ok)
        {
            return;
        }

        if (status == HostBridgeStatus.ObjectDisposed)
        {
            throw new ObjectDisposedException(objectName, message);
        }

        throw new HostBridgeException(status, message ?? "host bridge call failed");
    }
}

internal sealed class HostObject : IDisposable
{
    private readonly MockHostBridge _bridge;
    private bool _disposed;

    private HostObject(MockHostBridge bridge, ulong handle)
    {
        _bridge = bridge;
        Handle = handle;
    }

    public ulong Handle { get; }

    public bool IsAlive
    {
        get
        {
            EnsureNotDisposed();
            var status = _bridge.IsHandleAlive(Handle, out var alive, out var message);
            HostBridgeStatusConverter.ThrowIfFailed(status, message, nameof(HostObject));
            return alive;
        }
    }

    public static HostObject Create(MockHostBridge bridge, string typeName, string debugName)
    {
        var status = bridge.CreateHandle(typeName, debugName, out var handle, out var message);
        HostBridgeStatusConverter.ThrowIfFailed(status, message, nameof(HostObject));
        return new HostObject(bridge, handle);
    }

    public HostEventSubscription Subscribe(string eventName, Action callback)
    {
        EnsureAlive();
        var status = _bridge.SubscribeEvent(Handle, eventName, callback, out var token, out var message);
        HostBridgeStatusConverter.ThrowIfFailed(status, message, nameof(HostEventSubscription));
        return new HostEventSubscription(_bridge, token);
    }

    public void EnsureAlive()
    {
        if (!IsAlive)
        {
            throw new ObjectDisposedException(nameof(HostObject), "host object handle is no longer alive");
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        var status = _bridge.ReleaseHandle(Handle, out var message);
        _disposed = true;
        if (status != HostBridgeStatus.ObjectDisposed)
        {
            HostBridgeStatusConverter.ThrowIfFailed(status, message, nameof(HostObject));
        }
    }

    private void EnsureNotDisposed()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(HostObject), "host object wrapper has been disposed");
        }
    }
}

internal sealed class HostEventSubscription : IDisposable
{
    private readonly MockHostBridge _bridge;
    private bool _disposed;

    public HostEventSubscription(MockHostBridge bridge, ulong token)
    {
        _bridge = bridge;
        Token = token;
    }

    public ulong Token { get; }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        var status = _bridge.UnsubscribeEvent(Token, out var message);
        _disposed = true;
        HostBridgeStatusConverter.ThrowIfFailed(status, message, nameof(HostEventSubscription));
    }
}

internal static class HostDispatcher
{
    public static void PumpMainThread(MockHostBridge bridge)
    {
        var status = bridge.PumpMainThread(out _, out var message);
        HostBridgeStatusConverter.ThrowIfFailed(status, message, nameof(HostDispatcher));
    }
}

internal sealed class MockHostBridge
{
    private readonly List<HandleRecord> _handles = [];
    private readonly List<EventSubscriptionRecord> _subscriptions = [];
    private readonly Queue<Action> _dispatcher = new();
    private ulong _nextHandle = 100;
    private ulong _nextSubscription = 1;
    private ulong _lastSubscription;
    private string? _nextCreateFailure;

    public HostBridgeStatus CreateHandle(string typeName, string debugName, out ulong handle, out string? message)
    {
        if (_nextCreateFailure != null)
        {
            handle = 0;
            message = _nextCreateFailure;
            _nextCreateFailure = null;
            return HostBridgeStatus.HostFailure;
        }

        if (string.IsNullOrEmpty(typeName))
        {
            handle = 0;
            message = "host object type is required";
            return HostBridgeStatus.InvalidArgument;
        }

        handle = _nextHandle++;
        _handles.Add(new HandleRecord(handle, typeName, debugName));
        message = null;
        return HostBridgeStatus.Ok;
    }

    public HostBridgeStatus IsHandleAlive(ulong handle, out bool alive, out string? message)
    {
        var record = FindHandle(handle);
        alive = record != null && record.Alive && record.RefCount > 0;
        message = null;
        return HostBridgeStatus.Ok;
    }

    public HostBridgeStatus ReleaseHandle(ulong handle, out string? message)
    {
        var record = FindHandle(handle);
        if (record == null)
        {
            message = "host handle is unknown";
            return HostBridgeStatus.ObjectDisposed;
        }

        if (record.RefCount > 0)
        {
            record.RefCount--;
        }
        if (record.RefCount == 0)
        {
            record.Alive = false;
        }

        message = null;
        return HostBridgeStatus.Ok;
    }

    public HostBridgeStatus NotifyDestroyed(ulong handle)
    {
        var record = FindHandle(handle);
        if (record == null)
        {
            return HostBridgeStatus.ObjectDisposed;
        }

        record.Alive = false;
        record.RefCount = 0;
        return HostBridgeStatus.Ok;
    }

    public HostBridgeStatus SubscribeEvent(ulong sourceHandle, string eventName, Action callback, out ulong token, out string? message)
    {
        token = 0;
        var source = FindHandle(sourceHandle);
        if (source == null || !source.Alive || source.RefCount == 0)
        {
            message = "event source handle is no longer alive";
            return HostBridgeStatus.ObjectDisposed;
        }

        token = _nextSubscription++;
        _lastSubscription = token;
        _subscriptions.Add(new EventSubscriptionRecord(token, sourceHandle, eventName, callback));
        message = null;
        return HostBridgeStatus.Ok;
    }

    public HostBridgeStatus UnsubscribeEvent(ulong token, out string? message)
    {
        var subscription = FindSubscription(token);
        if (subscription != null)
        {
            subscription.Active = false;
        }

        message = null;
        return HostBridgeStatus.Ok;
    }

    public HostBridgeStatus TriggerEvent(ulong token)
    {
        var subscription = FindSubscription(token);
        if (subscription == null || !subscription.Active)
        {
            return HostBridgeStatus.Ok;
        }

        var source = FindHandle(subscription.SourceHandle);
        if (source == null || !source.Alive || source.RefCount == 0)
        {
            return HostBridgeStatus.ObjectDisposed;
        }

        _dispatcher.Enqueue(subscription.Callback);
        return HostBridgeStatus.Ok;
    }

    public void TriggerLastEvent()
    {
        TriggerEvent(_lastSubscription);
    }

    public HostBridgeStatus PumpMainThread(out int executed, out string? message)
    {
        executed = 0;
        while (_dispatcher.Count > 0)
        {
            var callback = _dispatcher.Dequeue();
            executed++;
            try
            {
                callback();
            }
            catch (Exception ex)
            {
                message = ex.Message;
                return HostBridgeStatus.ManagedException;
            }
        }

        message = null;
        return HostBridgeStatus.Ok;
    }

    public void FailNextCreate(string message)
    {
        _nextCreateFailure = message;
    }

    private HandleRecord? FindHandle(ulong handle)
    {
        for (int i = 0; i < _handles.Count; i++)
        {
            if (_handles[i].Handle == handle)
            {
                return _handles[i];
            }
        }

        return null;
    }

    private EventSubscriptionRecord? FindSubscription(ulong token)
    {
        for (int i = 0; i < _subscriptions.Count; i++)
        {
            if (_subscriptions[i].Token == token)
            {
                return _subscriptions[i];
            }
        }

        return null;
    }

    private sealed class HandleRecord
    {
        public HandleRecord(ulong handle, string typeName, string debugName)
        {
            Handle = handle;
            TypeName = typeName;
            DebugName = debugName;
        }

        public ulong Handle { get; }

        public string TypeName { get; }

        public string DebugName { get; }

        public int RefCount { get; set; } = 1;

        public bool Alive { get; set; } = true;
    }

    private sealed class EventSubscriptionRecord
    {
        public EventSubscriptionRecord(ulong token, ulong sourceHandle, string eventName, Action callback)
        {
            Token = token;
            SourceHandle = sourceHandle;
            EventName = eventName;
            Callback = callback;
        }

        public ulong Token { get; }

        public ulong SourceHandle { get; }

        public string EventName { get; }

        public Action Callback { get; }

        public bool Active { get; set; } = true;
    }
}
