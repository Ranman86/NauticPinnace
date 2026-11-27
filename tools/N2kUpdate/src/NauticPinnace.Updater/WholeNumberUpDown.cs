using System.Globalization;

namespace NauticPinnace.Updater;

/// <summary>
/// A NumericUpDown for whole numbers that reads typed text the same on every
/// Windows: ',' and '.' both count as the decimal point (numbers use a decimal
/// point, a typed comma keeps working), and the number is rounded to a whole
/// one. The stock control parses with the Windows culture, where '.' is a
/// thousands separator in German and ',' in English - "25.5" or "25,5" became
/// 255 - and it kept a German "25,5" as a fraction that the box never showed.
/// </summary>
internal sealed class WholeNumberUpDown : NumericUpDown
{
    protected override void OnTextBoxKeyPress(object? source, KeyPressEventArgs e)
    {
        // The stock control lets through only the culture's own decimal and
        // group separators; here both marks are allowed in every culture.
        if (e.KeyChar is '.' or ',') return;
        base.OnTextBoxKeyPress(source, e);
    }

    protected override void ValidateEditText()
    {
        string t = Text.Trim().Replace(',', '.');
        if (UserEdit && double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out double v) && double.IsFinite(v))
        {
            // UserEdit first: with it still set, the Value setter would parse
            // the text again the stock way.
            UserEdit = false;
            Value = (decimal)Math.Clamp(Math.Round(v, MidpointRounding.AwayFromZero), (double)Minimum, (double)Maximum);
            UpdateEditText();   // also when the value did not change: "30.4" shows as 30
            return;
        }
        base.ValidateEditText();
    }
}
