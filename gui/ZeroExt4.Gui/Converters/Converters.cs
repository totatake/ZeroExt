using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;
using ZeroExt4.Gui.Models;

namespace ZeroExt4.Gui.Converters
{
    public sealed class BoolToVisibilityConverter : IValueConverter
    {
        public bool Invert { get; set; }

        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            bool flag = value is bool b && b;
            if (Invert) flag = !flag;
            return flag ? Visibility.Visible : Visibility.Collapsed;
        }

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            bool visible = value is Visibility v && v == Visibility.Visible;
            return Invert ? !visible : visible;
        }
    }

    public sealed class InverseBoolConverter : IValueConverter
    {
        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
            => value is bool b ? !b : true;

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
            => value is bool b ? !b : false;
    }

    public sealed class NullOrEmptyToVisibilityConverter : IValueConverter
    {
        public bool ShowWhenNotEmpty { get; set; } = true;

        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            bool hasContent = value is string s ? !string.IsNullOrWhiteSpace(s) : value != null;
            bool show = ShowWhenNotEmpty ? hasContent : !hasContent;
            return show ? Visibility.Visible : Visibility.Collapsed;
        }

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
            => throw new NotSupportedException();
    }

    public sealed class CountToVisibilityConverter : IValueConverter
    {
        public bool ShowWhenZero { get; set; }

        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            int count = value switch
            {
                int i => i,
                long l => (int)l,
                _ => 0,
            };
            bool show = ShowWhenZero ? count == 0 : count > 0;
            return show ? Visibility.Visible : Visibility.Collapsed;
        }

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
            => throw new NotSupportedException();
    }

    public sealed class MountModeEqualConverter : IValueConverter
    {
        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            if (value is MountMode current && parameter is string targetStr &&
                Enum.TryParse<MountMode>(targetStr, out MountMode target))
            {
                return current == target;
            }
            return false;
        }

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            if (value is bool isChecked && isChecked && parameter is string targetStr &&
                Enum.TryParse<MountMode>(targetStr, out MountMode target))
            {
                return target;
            }
            return Binding.DoNothing;
        }
    }

    public sealed class JournalStateToBrushKeyConverter : IValueConverter
    {
        public bool IsBackground { get; set; }

        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            string state = (value as string ?? string.Empty).ToUpperInvariant();
            string key = state switch
            {
                "CLEAN" => IsBackground ? "SuccessBgBrush" : "SuccessBrush",
                "NEEDS_RECOVERY" => IsBackground ? "WarningBgBrush" : "WarningBrush",
                "HAS_ERRORS" => IsBackground ? "DangerBgBrush" : "DangerBrush",
                _ => IsBackground ? "NeutralBgBrush" : "NeutralBrush",
            };
            return Application.Current?.TryFindResource(key) ?? DependencyProperty.UnsetValue;
        }

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
            => throw new NotSupportedException();
    }

    public sealed class UpperStatusToBrushKeyConverter : IValueConverter
    {
        public bool IsBackground { get; set; }

        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            UpperDirCheckStatus status = value is UpperDirCheckStatus s ? s : UpperDirCheckStatus.None;
            string key = status switch
            {
                UpperDirCheckStatus.EmptyReady or UpperDirCheckStatus.MatchingExisting or UpperDirCheckStatus.WillCreate
                    => IsBackground ? "SuccessBgBrush" : "SuccessBrush",
                UpperDirCheckStatus.NonEmptyUnmarked
                    => IsBackground ? "WarningBgBrush" : "WarningBrush",
                UpperDirCheckStatus.MismatchedLower or UpperDirCheckStatus.InvalidPath
                    => IsBackground ? "DangerBgBrush" : "DangerBrush",
                _ => IsBackground ? "InfoBgBrush" : "InfoBrush",
            };
            return Application.Current?.TryFindResource(key) ?? DependencyProperty.UnsetValue;
        }

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
            => throw new NotSupportedException();
    }

    public sealed class ByteSizeConverter : IValueConverter
    {
        public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        {
            return value switch
            {
                ulong u => ByteFormatter.Format(u),
                long l when l >= 0 => ByteFormatter.Format((ulong)l),
                _ => "0 B",
            };
        }

        public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
            => throw new NotSupportedException();
    }
}
