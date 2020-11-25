using System;
using System.Collections.Generic;
using System.Configuration;
using System.Linq;
using System.Text;
using System.Windows;

namespace EpgTimer
{
    /// <summary>
    /// App.xaml の相互作用ロジック
    /// </summary>
    public partial class App : Application
    {
        static App()
        {
            // Shift_JIS等のコードページの登録
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        }
    }
}
