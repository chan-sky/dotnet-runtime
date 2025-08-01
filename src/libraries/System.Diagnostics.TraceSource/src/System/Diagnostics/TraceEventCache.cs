// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections;
using System.Globalization;
using System.Text;
using System.Threading;

namespace System.Diagnostics
{
    public partial class TraceEventCache
    {
        private long _timeStamp = -1;
        private DateTime _dateTime = DateTime.MinValue;

        public DateTime DateTime
        {
            get
            {
                if (_dateTime == DateTime.MinValue)
                    _dateTime = DateTime.UtcNow;
                return _dateTime;
            }
        }

        public int ProcessId
        {
            get
            {
                return Environment.ProcessId;
            }
        }

        public string ThreadId
        {
            get
            {
                return Environment.CurrentManagedThreadId.ToString(CultureInfo.InvariantCulture);
            }
        }

        public long Timestamp
        {
            get
            {
                if (_timeStamp == -1)
                    _timeStamp = Stopwatch.GetTimestamp();
                return _timeStamp;
            }
        }

        public string Callstack => field ??= Environment.StackTrace;

        public Stack LogicalOperationStack
        {
            get
            {
                return Trace.CorrelationManager.LogicalOperationStack;
            }
        }
    }
}
