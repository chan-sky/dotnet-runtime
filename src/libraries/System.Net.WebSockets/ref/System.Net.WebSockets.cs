// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// ------------------------------------------------------------------------------
// Changes to this file must follow the https://aka.ms/api-review process.
// ------------------------------------------------------------------------------

namespace System.Net.WebSockets
{
    public readonly partial struct ValueWebSocketReceiveResult
    {
        private readonly int _dummyPrimitive;
        public ValueWebSocketReceiveResult(int count, System.Net.WebSockets.WebSocketMessageType messageType, bool endOfMessage) { throw null; }
        public int Count { get { throw null; } }
        public bool EndOfMessage { get { throw null; } }
        public System.Net.WebSockets.WebSocketMessageType MessageType { get { throw null; } }
    }
    public abstract partial class WebSocket : System.IDisposable
    {
        protected WebSocket() { }
        public abstract System.Net.WebSockets.WebSocketCloseStatus? CloseStatus { get; }
        public abstract string? CloseStatusDescription { get; }
        public static System.TimeSpan DefaultKeepAliveInterval { get { throw null; } }
        public abstract System.Net.WebSockets.WebSocketState State { get; }
        public abstract string? SubProtocol { get; }
        public abstract void Abort();
        public abstract System.Threading.Tasks.Task CloseAsync(System.Net.WebSockets.WebSocketCloseStatus closeStatus, string? statusDescription, System.Threading.CancellationToken cancellationToken);
        public abstract System.Threading.Tasks.Task CloseOutputAsync(System.Net.WebSockets.WebSocketCloseStatus closeStatus, string? statusDescription, System.Threading.CancellationToken cancellationToken);
        public static System.ArraySegment<byte> CreateClientBuffer(int receiveBufferSize, int sendBufferSize) { throw null; }
        [System.ComponentModel.EditorBrowsableAttribute(System.ComponentModel.EditorBrowsableState.Never)]
        public static System.Net.WebSockets.WebSocket CreateClientWebSocket(System.IO.Stream innerStream, string? subProtocol, int receiveBufferSize, int sendBufferSize, System.TimeSpan keepAliveInterval, bool useZeroMaskingKey, System.ArraySegment<byte> internalBuffer) { throw null; }
        public static System.Net.WebSockets.WebSocket CreateFromStream(System.IO.Stream stream, bool isServer, string? subProtocol, System.TimeSpan keepAliveInterval) { throw null; }
        public static System.Net.WebSockets.WebSocket CreateFromStream(System.IO.Stream stream, System.Net.WebSockets.WebSocketCreationOptions options) { throw null; }
        public static System.ArraySegment<byte> CreateServerBuffer(int receiveBufferSize) { throw null; }
        public abstract void Dispose();
        [System.ComponentModel.EditorBrowsableAttribute(System.ComponentModel.EditorBrowsableState.Never)]
        [System.ObsoleteAttribute("This API supports the .NET Framework infrastructure and is not intended to be used directly from your code.")]
        public static bool IsApplicationTargeting45() { throw null; }
        protected static bool IsStateTerminal(System.Net.WebSockets.WebSocketState state) { throw null; }
        public abstract System.Threading.Tasks.Task<System.Net.WebSockets.WebSocketReceiveResult> ReceiveAsync(System.ArraySegment<byte> buffer, System.Threading.CancellationToken cancellationToken);
        public virtual System.Threading.Tasks.ValueTask<System.Net.WebSockets.ValueWebSocketReceiveResult> ReceiveAsync(System.Memory<byte> buffer, System.Threading.CancellationToken cancellationToken) { throw null; }
        [System.ComponentModel.EditorBrowsableAttribute(System.ComponentModel.EditorBrowsableState.Never)]
        [System.ObsoleteAttribute("This API supports the .NET Framework infrastructure and is not intended to be used directly from your code.")]
        public static void RegisterPrefixes() { }
        public abstract System.Threading.Tasks.Task SendAsync(System.ArraySegment<byte> buffer, System.Net.WebSockets.WebSocketMessageType messageType, bool endOfMessage, System.Threading.CancellationToken cancellationToken);
        public virtual System.Threading.Tasks.ValueTask SendAsync(System.ReadOnlyMemory<byte> buffer, System.Net.WebSockets.WebSocketMessageType messageType, bool endOfMessage, System.Threading.CancellationToken cancellationToken) { throw null; }
        public virtual System.Threading.Tasks.ValueTask SendAsync(System.ReadOnlyMemory<byte> buffer, System.Net.WebSockets.WebSocketMessageType messageType, System.Net.WebSockets.WebSocketMessageFlags messageFlags, System.Threading.CancellationToken cancellationToken) { throw null; }
        protected static void ThrowOnInvalidState(System.Net.WebSockets.WebSocketState state, params System.Net.WebSockets.WebSocketState[] validStates) { }
    }
    public partial class WebSocketStream : System.IO.Stream
    {
        private protected WebSocketStream() { }
        public override System.IAsyncResult BeginWrite(byte[] buffer, int offset, int count, System.AsyncCallback? callback, object? state) { throw null; }
        public override System.IAsyncResult BeginRead(byte[] buffer, int offset, int count, System.AsyncCallback? callback, object? state) { throw null; }
        public override bool CanRead { get { throw null; } }
        public override bool CanSeek { get { throw null; } }
        public override bool CanWrite { get { throw null; } }
        public static System.Net.WebSockets.WebSocketStream Create(WebSocket webSocket, WebSocketMessageType writeMessageType, bool ownsWebSocket = false) { throw null; }
        public static System.Net.WebSockets.WebSocketStream Create(WebSocket webSocket, WebSocketMessageType writeMessageType, TimeSpan closeTimeout) { throw null; }
        public static System.Net.WebSockets.WebSocketStream CreateWritableMessageStream(WebSocket webSocket, WebSocketMessageType writeMessageType) { throw null; }
        public static System.Net.WebSockets.WebSocketStream CreateReadableMessageStream(WebSocket webSocket) { throw null; }
        protected override void Dispose(bool disposing) { }
        public override System.Threading.Tasks.ValueTask DisposeAsync() { throw null; }
        public override int EndRead(System.IAsyncResult asyncResult) { throw null; }
        public override void EndWrite(IAsyncResult asyncResult) { }
        public override void Flush() { }
        public override System.Threading.Tasks.Task FlushAsync(System.Threading.CancellationToken cancellationToken) { throw null; }
        public override long Length { get { throw null; } }
        public override long Position { get { throw null; } set { } }
        public override int Read(byte[] buffer, int offset, int count) { throw null; }
        public override System.Threading.Tasks.Task<int> ReadAsync(byte[] buffer, int offset, int count, System.Threading.CancellationToken cancellationToken) { throw null; }
        public override System.Threading.Tasks.ValueTask<int> ReadAsync(System.Memory<byte> buffer, System.Threading.CancellationToken cancellationToken = default) { throw null; }
        public override long Seek(long offset, System.IO.SeekOrigin origin) { throw null; }
        public override void SetLength(long value) { }
        public System.Net.WebSockets.WebSocket WebSocket { get { throw null; } }
        public override void Write(byte[] buffer, int offset, int count) { throw null; }
        public override System.Threading.Tasks.Task WriteAsync(byte[] buffer, int offset, int count, System.Threading.CancellationToken cancellationToken) { throw null; }
        public override System.Threading.Tasks.ValueTask WriteAsync(System.ReadOnlyMemory<byte> buffer, System.Threading.CancellationToken cancellationToken = default) { throw null; }
    }
    public enum WebSocketCloseStatus
    {
        NormalClosure = 1000,
        EndpointUnavailable = 1001,
        ProtocolError = 1002,
        InvalidMessageType = 1003,
        Empty = 1005,
        InvalidPayloadData = 1007,
        PolicyViolation = 1008,
        MessageTooBig = 1009,
        MandatoryExtension = 1010,
        InternalServerError = 1011,
    }
    public abstract partial class WebSocketContext
    {
        protected WebSocketContext() { }
        public abstract System.Net.CookieCollection CookieCollection { get; }
        public abstract System.Collections.Specialized.NameValueCollection Headers { get; }
        public abstract bool IsAuthenticated { get; }
        public abstract bool IsLocal { get; }
        public abstract bool IsSecureConnection { get; }
        public abstract string Origin { get; }
        public abstract System.Uri RequestUri { get; }
        public abstract string SecWebSocketKey { get; }
        public abstract System.Collections.Generic.IEnumerable<string> SecWebSocketProtocols { get; }
        public abstract string SecWebSocketVersion { get; }
        public abstract System.Security.Principal.IPrincipal? User { get; }
        public abstract System.Net.WebSockets.WebSocket WebSocket { get; }
    }
    public enum WebSocketError
    {
        Success = 0,
        InvalidMessageType = 1,
        Faulted = 2,
        NativeError = 3,
        NotAWebSocket = 4,
        UnsupportedVersion = 5,
        UnsupportedProtocol = 6,
        HeaderError = 7,
        ConnectionClosedPrematurely = 8,
        InvalidState = 9,
    }
    public sealed partial class WebSocketException : System.ComponentModel.Win32Exception
    {
        public WebSocketException() { }
        public WebSocketException(int nativeError) { }
        public WebSocketException(int nativeError, System.Exception? innerException) { }
        public WebSocketException(int nativeError, string? message) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error, System.Exception? innerException) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error, int nativeError) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error, int nativeError, System.Exception? innerException) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error, int nativeError, string? message) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error, int nativeError, string? message, System.Exception? innerException) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error, string? message) { }
        public WebSocketException(System.Net.WebSockets.WebSocketError error, string? message, System.Exception? innerException) { }
        public WebSocketException(string? message) { }
        public WebSocketException(string? message, System.Exception? innerException) { }
        public override int ErrorCode { get { throw null; } }
        public System.Net.WebSockets.WebSocketError WebSocketErrorCode { get { throw null; } }
        [System.ObsoleteAttribute("This API supports obsolete formatter-based serialization. It should not be called or extended by application code.", DiagnosticId = "SYSLIB0051", UrlFormat = "https://aka.ms/dotnet-warnings/{0}")]
        [System.ComponentModel.EditorBrowsableAttribute(System.ComponentModel.EditorBrowsableState.Never)]
        public override void GetObjectData(System.Runtime.Serialization.SerializationInfo info, System.Runtime.Serialization.StreamingContext context) { }
    }
    public enum WebSocketMessageType
    {
        Text = 0,
        Binary = 1,
        Close = 2,
    }
    public partial class WebSocketReceiveResult
    {
        public WebSocketReceiveResult(int count, System.Net.WebSockets.WebSocketMessageType messageType, bool endOfMessage) { }
        public WebSocketReceiveResult(int count, System.Net.WebSockets.WebSocketMessageType messageType, bool endOfMessage, System.Net.WebSockets.WebSocketCloseStatus? closeStatus, string? closeStatusDescription) { }
        public System.Net.WebSockets.WebSocketCloseStatus? CloseStatus { get { throw null; } }
        public string? CloseStatusDescription { get { throw null; } }
        public int Count { get { throw null; } }
        public bool EndOfMessage { get { throw null; } }
        public System.Net.WebSockets.WebSocketMessageType MessageType { get { throw null; } }
    }
    public enum WebSocketState
    {
        None = 0,
        Connecting = 1,
        Open = 2,
        CloseSent = 3,
        CloseReceived = 4,
        Closed = 5,
        Aborted = 6,
    }
    public sealed partial class WebSocketCreationOptions
    {
        public bool IsServer { get { throw null; } set { } }
        public string? SubProtocol { get { throw null; } set { } }
        public System.TimeSpan KeepAliveInterval { get { throw null; } set { } }
        public System.TimeSpan KeepAliveTimeout { get { throw null; } set { } }
        public System.Net.WebSockets.WebSocketDeflateOptions? DangerousDeflateOptions { get { throw null; } set { } }
    }
    public sealed partial class WebSocketDeflateOptions
    {
        public int ClientMaxWindowBits { get { throw null; } set { } }
        public bool ClientContextTakeover { get { throw null; } set { } }
        public int ServerMaxWindowBits { get { throw null; } set { } }
        public bool ServerContextTakeover { get { throw null; } set { } }
    }
    [Flags]
    public enum WebSocketMessageFlags
    {
        None = 0,
        EndOfMessage = 1,
        DisableCompression = 2
    }
}
