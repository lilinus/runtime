// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Globalization;
using System.IO;
using System.Linq;
using Xunit;
using Microsoft.DotNet.XUnitExtensions;

namespace System.Diagnostics.Tests
{
    public class EventLogTests : FileCleanupTestBase
    {
        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void EventLogReinitializationException()
        {
            using (EventLog eventLog = new EventLog())
            {
                eventLog.BeginInit();
                Assert.Throws<InvalidOperationException>(() => eventLog.BeginInit());
                eventLog.EndInit();
            }
        }

        [Trait(XunitConstants.Category, XunitConstants.IgnoreForCI)] // Unreliable Win32 API call
        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void ClearLog()
        {
            string log = "ClearTest";
            string source = "Source_" + nameof(ClearLog);

            try
            {
                EventLog.CreateEventSource(source, log);
                using (EventLog eventLog = new EventLog())
                {
                    eventLog.Source = source;
                    Helpers.Retry(() => eventLog.Clear());
                    Assert.Equal(0, Helpers.Retry((() => eventLog.Entries.Count)));
                    Helpers.Retry(() => eventLog.WriteEntry("Writing to event log."));
                    Helpers.WaitForEventLog(eventLog, 1);
                    Assert.Equal(1, Helpers.Retry((() => eventLog.Entries.Count)));
                }
            }
            finally
            {
                EventLog.DeleteEventSource(source);
                Helpers.RetrySilently(() => EventLog.Delete(log));
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void ApplicationEventLog_Count()
        {
            using (EventLog eventLog = new EventLog("Application"))
            {
                Assert.InRange(Helpers.Retry((() => eventLog.Entries.Count)), 1, int.MaxValue);
            }
        }

        [Trait(XunitConstants.Category, XunitConstants.IgnoreForCI)] // Unreliable Win32 API call
        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void DeleteLog()
        {
            string log = "DeleteTest";
            string source = "Source_" + nameof(DeleteLog);

            try
            {
                EventLog.CreateEventSource(source, log);
                Assert.True(EventLog.Exists(log));
            }
            finally
            {
                EventLog.DeleteEventSource(source);
                Helpers.Retry(() => EventLog.Delete(log)); // unlike other tests, throw if delete fails
                Assert.False(EventLog.Exists(log));
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void CheckLogName_Get()
        {
            using (EventLog eventLog = new EventLog("Application"))
            {
                Assert.False(string.IsNullOrEmpty(eventLog.LogDisplayName));
                if (CultureInfo.CurrentCulture.Name.Split('-')[0] == "en")
                    Assert.Equal("Application", eventLog.LogDisplayName);
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void CheckMachineName_Get()
        {
            using (EventLog eventLog = new EventLog("Application"))
            {
                Assert.Equal(".", eventLog.MachineName);
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void GetLogDisplayName_NotSet_Throws()
        {
            using (EventLog eventLog = new EventLog())
            {
                eventLog.Log = Guid.NewGuid().ToString("N");
                Assert.Throws<InvalidOperationException>(() => eventLog.LogDisplayName);
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void GetLogDisplayName_Set()
        {
            using (EventLog eventLog = new EventLog())
            {
                eventLog.Log = "Application";

                Assert.False(string.IsNullOrEmpty(eventLog.LogDisplayName));
                if (CultureInfo.CurrentCulture.Name.Split('-')[0] == "en")
                    Assert.Equal("Application", eventLog.LogDisplayName);
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void EventLogs_Get()
        {
            Assert.Throws<ArgumentException>(() => EventLog.GetEventLogs(""));
            EventLog[] eventLogCollection = EventLog.GetEventLogs();
            Assert.Contains(eventLogCollection, eventlog => eventlog.Log.Equals("Application"));
            Assert.Contains(eventLogCollection, eventlog => eventlog.Log.Equals("System"));
        }

        [Trait(XunitConstants.Category, XunitConstants.IgnoreForCI)] // Unreliable Win32 API call
        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void GetMaxKilobytes_Set()
        {
            string source = "Source_" + nameof(GetMaxKilobytes_Set);
            string log = "maxKilobytesLog";

            try
            {
                EventLog.CreateEventSource(source, log);
                using (EventLog eventLog = new EventLog())
                {
                    eventLog.Source = source;
                    eventLog.MaximumKilobytes = 0x400;
                    Assert.Equal(0x400, eventLog.MaximumKilobytes);
                }
            }
            finally
            {
                EventLog.DeleteEventSource(source);
                Helpers.RetrySilently(() => EventLog.Delete(log));
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void MaxKilobytesOutOfRangeException()
        {
            using (EventLog eventLog = new EventLog())
            {
                eventLog.Log = "Application";
                Assert.Throws<ArgumentOutOfRangeException>(() => eventLog.MaximumKilobytes = 2);
                Assert.Throws<ArgumentOutOfRangeException>(() => eventLog.MaximumKilobytes = 0x3FFFC1);
            }
        }

        [Trait(XunitConstants.Category, XunitConstants.IgnoreForCI)] // Unreliable Win32 API call
        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void OverflowAndRetention_Set()
        {
            string source = "Source_" + nameof(OverflowAndRetention_Set);
            string log = "Overflow_Set";

            try
            {
                EventLog.CreateEventSource(source, log);
                using (EventLog eventLog = new EventLog())
                {
                    eventLog.Source = source;

                    // The second argument is only used when the overflow policy is set to OverWrite Older
                    eventLog.ModifyOverflowPolicy(OverflowAction.DoNotOverwrite, 1);
                    Assert.Equal(OverflowAction.DoNotOverwrite, eventLog.OverflowAction);

                    // -1 means overflow action is donot OverWrite
                    Assert.Equal(-1, eventLog.MinimumRetentionDays);
                }
            }
            finally
            {
                EventLog.DeleteEventSource(source);
                Helpers.RetrySilently(() => EventLog.Delete(log));
            }
        }

        [Trait(XunitConstants.Category, XunitConstants.IgnoreForCI)] // Unreliable Win32 API call
        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void Overflow_OverWriteOlderAndRetention_Set()
        {
            string source = "Source_" + nameof(OverflowAndRetention_Set);
            string log = "Overflow_Set";
            int retentionDays = 30; // A number between 0 and 365 should work

            try
            {
                EventLog.CreateEventSource(source, log);
                using (EventLog eventLog = new EventLog())
                {
                    eventLog.Source = source;

                    // The second argument is only used when the overflow policy is set to OverWrite Older
                    eventLog.ModifyOverflowPolicy(OverflowAction.OverwriteOlder, retentionDays);
                    Assert.Equal(OverflowAction.OverwriteOlder, eventLog.OverflowAction);
                    Assert.Equal(retentionDays, eventLog.MinimumRetentionDays);
                }
            }
            finally
            {
                EventLog.DeleteEventSource(source);
                Helpers.RetrySilently(() => EventLog.Delete(log));
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void OverflowAndRetentionDaysOutOfRange()
        {
            using (EventLog eventLog = new EventLog())
            {
                eventLog.Log = "Application";
                Assert.Throws<ArgumentOutOfRangeException>(() => eventLog.ModifyOverflowPolicy(OverflowAction.OverwriteOlder, 400));
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void MachineName_Set()
        {
            string source = "Source_" + nameof(MachineName_Set);
            using (EventLog eventLog = new EventLog())
            {
                eventLog.Log = "Application";
                eventLog.MachineName = Environment.MachineName.ToLowerInvariant();
                try
                {
                    EventLog.CreateEventSource(source, eventLog.LogDisplayName);
                    Assert.Equal(eventLog.MachineName, Environment.MachineName.ToLowerInvariant());
                    Assert.True(EventLog.SourceExists(source, Environment.MachineName.ToLowerInvariant()));
                }
                finally
                {
                    EventLog.DeleteEventSource(source);
                }
            }
        }

        [Trait(XunitConstants.Category, XunitConstants.IgnoreForCI)] // Unreliable Win32 API call
        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void RegisterDisplayLogName()
        {
            string log = "DisplayName";
            string source = "Source_" + nameof(RegisterDisplayLogName);
            string messageFile = GetTestFilePath();
            long DisplayNameMsgId = 42; // It could be any number
            EventSourceCreationData sourceData = new EventSourceCreationData(source, log);

            try
            {
                EventLog.CreateEventSource(sourceData);
                log = EventLog.LogNameFromSourceName(source, ".");
                using (EventLog eventLog = new EventLog(log, ".", source))
                {
                    if (messageFile.Length > 0)
                    {
                        eventLog.RegisterDisplayName(messageFile, DisplayNameMsgId);
                    }
                    Assert.Equal(log, eventLog.LogDisplayName);
                }
            }
            finally
            {
                EventLog.DeleteEventSource(source);
                Helpers.RetrySilently(() => EventLog.Delete(log));
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void InvalidFormatOrNullLogName()
        {
            Assert.Throws<ArgumentNullException>(() => new EventLog(null));
            Assert.Throws<ArgumentException>(() => new EventLog("?"));
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        public void EventLog_EnableRaisingEvents_DefaultFalse()
        {
            using (EventLog eventLog = new EventLog("log"))
            {
                Assert.False(eventLog.EnableRaisingEvents);
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void InvalidFormatOrNullDeleteLogName()
        {
            Assert.Throws<ArgumentException>(() => EventLog.Delete(null));
            Assert.Throws<InvalidOperationException>(() => EventLog.Delete("?"));
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void InvalidLogExistsLogName()
        {
            Assert.False(EventLog.Exists(null));
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void InvalidMachineName()
        {
            Assert.Throws<ArgumentException>(() => EventLog.Exists("Application", ""));
            Assert.Throws<ArgumentException>(() => EventLog.Delete("", ""));
            Assert.Throws<ArgumentException>(() => EventLog.DeleteEventSource("", ""));
        }

        [Trait(XunitConstants.Category, XunitConstants.IgnoreForCI)] // Unreliable Win32 API call
        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void LogDisplayNameDefault()
        {
            string source = "Source_" + nameof(LogDisplayNameDefault);
            string log = "MyLogDisplay";
            try
            {
                EventLog.CreateEventSource(source, log);
                using (EventLog eventlog = new EventLog())
                {
                    eventlog.Source = source;
                    Assert.Equal(log, eventlog.LogDisplayName);
                }
            }
            finally
            {
                EventLog.DeleteEventSource(source);
                Helpers.RetrySilently(() => EventLog.Delete(log));
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        public void GetMessageUsingEventMessageDLL()
        {
            if (CultureInfo.CurrentCulture.ToString() != "en-US")
            {
                return;
            }

            using (EventLog eventlog = new EventLog("Security"))
            {
                eventlog.Source = "Security";
                Assert.Contains("", eventlog.Entries.LastOrDefault()?.Message ?? "");
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.IsElevatedAndSupportsEventLogs))]
        [SkipOnTargetFramework(TargetFrameworkMonikers.NetFramework)]
        public void GetEventLogEntriesTest()
        {
            foreach (var eventLog in EventLog.GetEventLogs())
            {
                // Accessing eventlog properties should not throw.
                Assert.True(Helpers.Retry(() => eventLog.Entries.Count) >= 0);
            }
        }

        [ConditionalFact(typeof(Helpers), nameof(Helpers.SupportsEventLogs))]
        [SkipOnTargetFramework(TargetFrameworkMonikers.NetFramework)]
        public void GetEventLogContainsSecurityLogTest()
        {
            EventLog[] eventlogs = EventLog.GetEventLogs();
            Assert.Contains("Security", eventlogs.Select(t => t.Log), StringComparer.OrdinalIgnoreCase);
        }

        private static bool IsNetCoreAndNotElevatedAndSupportsEventLogs => PlatformDetection.IsNetCore && Helpers.NotElevatedAndSupportsEventLogs;

        [ConditionalFact(nameof(IsNetCoreAndNotElevatedAndSupportsEventLogs))]
        public void CheckTheExceptionMessageContent()
        {
            using (EventLog myLog = new EventLog("Application", ".", "InvalidNotExistingSource"))
            {
                var exception = Record.Exception(() => myLog.WriteEntry("Test Entry"));
                Assert.True(exception is System.Security.SecurityException, $"Expected to get a SecurityException, but got {(exception is null ? "null" : exception.GetType())} instead.");
                Assert.Contains("InvalidNotExistingSource", exception.Message, StringComparison.Ordinal);
                Assert.Contains(" .", exception.Message, StringComparison.Ordinal); // machine name check
            }
        }
    }
}
