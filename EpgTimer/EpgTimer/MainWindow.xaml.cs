﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;
using System.Threading; //紅

namespace EpgTimer
{
    /// <summary>
    /// MainWindow.xaml の相互作用ロジック
    /// </summary>
    public partial class MainWindow : Window
    {
        private System.Threading.Mutex mutex;

        private TaskTrayClass taskTray = new TaskTrayClass();
        private Dictionary<string, Button> buttonList = new Dictionary<string, Button>();

        private PipeServer pipeServer = null;
        private string pipeName = "\\\\.\\pipe\\EpgTimerGUI_Ctrl_BonPipe_";
        private string pipeEventName = "Global\\EpgTimerGUI_Ctrl_BonConnect_";

        private bool closeFlag = false;
        private bool initExe = false;

        private bool needUnRegist = true;

        public MainWindow()
        {
            string appName = System.IO.Path.GetFileNameWithoutExtension(System.Reflection.Assembly.GetEntryAssembly().Location);
            CommonManager.Instance.NWMode = appName.StartsWith("EpgTimerNW", StringComparison.OrdinalIgnoreCase);

            Settings.LoadFromXmlFile(CommonManager.Instance.NWMode);
            if (CommonManager.Instance.NWMode == true)
            {
                CommonManager.Instance.DB.SetNoAutoReloadEPG(Settings.Instance.NgAutoEpgLoadNW);
            }
            CommonManager.Instance.ReloadCustContentColorList();

            if (Settings.Instance.NoStyle == 0)
            {
                if (System.IO.File.Exists(System.Reflection.Assembly.GetEntryAssembly().Location + ".rd.xaml"))
                {
                    //ResourceDictionaryを定義したファイルがあるので本体にマージする
                    try
                    {
                        App.Current.Resources.MergedDictionaries.Add(
                            (ResourceDictionary)System.Windows.Markup.XamlReader.Load(
                                System.Xml.XmlReader.Create(System.Reflection.Assembly.GetEntryAssembly().Location + ".rd.xaml")));
                    }
                    catch (Exception ex)
                    {
                        MessageBox.Show(ex.ToString());
                    }
                }
                else
                {
                    //既定のテーマ(Aero)をマージする
                    App.Current.Resources.MergedDictionaries.Add(
                        Application.LoadComponent(new Uri("/PresentationFramework.Aero, Version=4.0.0.0, Culture=neutral, PublicKeyToken=31bf3856ad364e35;component/themes/aero.normalcolor.xaml", UriKind.Relative)) as ResourceDictionary
                        );
                }
            }

            mutex = new Mutex(false, CommonManager.Instance.NWMode ? "Global\\EpgTimer_BonNW" + appName.Substring(10).ToUpper() : "Global\\EpgTimer_Bon2");
            if (!mutex.WaitOne(0, false))
            {
                CheckCmdLine();

                mutex.Close();
                mutex = null;

                closeFlag = true;
                Close();
                return;
            }

            if (CommonManager.Instance.NWMode == false)
            {
                bool startExe = true;
                try
                {
                    using (Mutex.OpenExisting("Global\\EpgTimer_Bon_Service")) { }
                }
                catch (WaitHandleCannotBeOpenedException)
                {
                    //二重起動抑止Mutexが存在しないのでEpgTimerSrvを起動する
                    //サービスモード時は何もしない(遅延開始かもしれないので失敗にはしない)
                    if (System.ServiceProcess.ServiceController.GetServices().All(sc => string.Compare(sc.ServiceName, "EpgTimer Service", true) != 0))
                    {
                        startExe = false;
                    }
                }
                catch { }
                try
                {
                    if (startExe == false)
                    {
                        String exePath = System.IO.Path.Combine(SettingPath.ModulePath, "EpgTimerSrv.exe");
                        System.Diagnostics.Process process = System.Diagnostics.Process.Start(exePath);
                        startExe = true;
                    }
                }
                catch (Exception ex)
                {
                    MessageBox.Show(ex.Message + "\r\n" + ex.StackTrace);
                    startExe = false;
                }

                if (startExe == false)
                {
                    MessageBox.Show("EpgTimerSrv.exeの起動ができませんでした");
                    closeFlag = true;
                    Close();
                    return;
                }
            }

            InitializeComponent();

            Title = appName;
            initExe = true;

            try
            {
                if (Settings.Instance.WakeMin == true)
                {
                    if (Settings.Instance.ShowTray && Settings.Instance.MinHide)
                    {
                        this.Visibility = System.Windows.Visibility.Hidden;
                    }
                    else
                    {
                        Dispatcher.BeginInvoke(new Action(() =>
                        {
                            this.WindowState = System.Windows.WindowState.Minimized;
                        }));
                    }
                }

                //ウインドウ位置の復元
                if (Settings.Instance.MainWndTop != -100)
                {
                    this.Top = Settings.Instance.MainWndTop;
                }
                if (Settings.Instance.MainWndLeft != -100)
                {
                    this.Left = Settings.Instance.MainWndLeft;
                }
                if (Settings.Instance.MainWndWidth != -100)
                {
                    this.Width = Settings.Instance.MainWndWidth;
                }
                if (Settings.Instance.MainWndHeight != -100)
                {
                    this.Height = Settings.Instance.MainWndHeight;
                }
                this.WindowState = Settings.Instance.LastWindowState;


                //上のボタン
                Button settingButton = new Button();
                settingButton.MinWidth = 75;
                settingButton.Margin = new Thickness(2, 2, 2, 5);
                settingButton.Click += new RoutedEventHandler(settingButton_Click);
                settingButton.Content = "設定";
                buttonList.Add("設定", settingButton);

                Button searchButton = new Button();
                searchButton.MinWidth = 75;
                searchButton.Margin = new Thickness(2, 2, 2, 5);
                searchButton.Click += new RoutedEventHandler(searchButton_Click);
                searchButton.Content = "検索";
                buttonList.Add("検索", searchButton);

                Button closeButton = new Button();
                closeButton.MinWidth = 75;
                closeButton.Margin = new Thickness(2, 2, 2, 5);
                closeButton.Click += new RoutedEventHandler(closeButton_Click);
                closeButton.Content = "終了";
                buttonList.Add("終了", closeButton);

                Button stanbyButton = new Button();
                stanbyButton.MinWidth = 75;
                stanbyButton.Margin = new Thickness(2, 2, 2, 5);
                stanbyButton.Click += new RoutedEventHandler(standbyButton_Click);
                stanbyButton.Content = "スタンバイ";
                buttonList.Add("スタンバイ", stanbyButton);

                Button suspendButton = new Button();
                suspendButton.MinWidth = 75;
                suspendButton.Margin = new Thickness(2, 2, 2, 5);
                suspendButton.Click += new RoutedEventHandler(suspendButton_Click);
                suspendButton.Content = "休止";
                buttonList.Add("休止", suspendButton);

                Button epgCapButton = new Button();
                epgCapButton.MinWidth = 75;
                epgCapButton.Margin = new Thickness(2, 2, 2, 5);
                epgCapButton.Click += new RoutedEventHandler(epgCapButton_Click);
                epgCapButton.Content = "EPG取得";
                buttonList.Add("EPG取得", epgCapButton);

                Button epgReloadButton = new Button();
                epgReloadButton.MinWidth = 75;
                epgReloadButton.Margin = new Thickness(2, 2, 2, 5);
                epgReloadButton.Click += new RoutedEventHandler(epgReloadButton_Click);
                epgReloadButton.Content = "EPG再読み込み";
                buttonList.Add("EPG再読み込み", epgReloadButton);

                Button custum1Button = new Button();
                custum1Button.MinWidth = 75;
                custum1Button.Margin = new Thickness(2, 2, 2, 5);
                custum1Button.Click += new RoutedEventHandler(custum1Button_Click);
                custum1Button.Content = "カスタム１";
                buttonList.Add("カスタム１", custum1Button);

                Button custum2Button = new Button();
                custum2Button.MinWidth = 75;
                custum2Button.Margin = new Thickness(2, 2, 2, 5);
                custum2Button.Click += new RoutedEventHandler(custum2Button_Click);
                custum2Button.Content = "カスタム２";
                buttonList.Add("カスタム２", custum2Button);

                Button nwTVEndButton = new Button();
                nwTVEndButton.MinWidth = 75;
                nwTVEndButton.Margin = new Thickness(2, 2, 2, 5);
                nwTVEndButton.Click += new RoutedEventHandler(nwTVEndButton_Click);
                nwTVEndButton.Content = "NetworkTV終了";
                buttonList.Add("NetworkTV終了", nwTVEndButton);

                Button logViewButton = new Button();
                logViewButton.MinWidth = 75;
                logViewButton.Margin = new Thickness(2, 2, 2, 5);
                logViewButton.Click += new RoutedEventHandler(logViewButton_Click);
                logViewButton.Content = "情報通知ログ";
                buttonList.Add("情報通知ログ", logViewButton);

                Button connectButton = new Button();
                connectButton.MinWidth = 75;
                connectButton.Margin = new Thickness(2, 2, 2, 5);
                connectButton.Click += new RoutedEventHandler(connectButton_Click);
                connectButton.Content = "再接続";
                buttonList.Add("再接続", connectButton);

                ResetButtonView();

                if (CommonManager.Instance.NWMode == false)
                {
                    pipeServer = new PipeServer();
                    pipeName += System.Diagnostics.Process.GetCurrentProcess().Id.ToString();
                    pipeEventName += System.Diagnostics.Process.GetCurrentProcess().Id.ToString();
                    //コールバックは別スレッドかもしれないので設定は予めキャプチャする
                    uint execBat = Settings.Instance.ExecBat;
                    pipeServer.StartServer(pipeEventName, pipeName, (c, r) => OutsideCmdCallback(c, r, false, execBat));

                    for (int i = 0; i < 150; i++)
                    {
                        if (CommonManager.CreateSrvCtrl().SendRegistGUI((uint)System.Diagnostics.Process.GetCurrentProcess().Id) == ErrCode.CMD_SUCCESS)
                        {
                            byte[] binData;
                            if (CommonManager.CreateSrvCtrl().SendFileCopy("ChSet5.txt", out binData) == ErrCode.CMD_SUCCESS)
                            {
                                ChSet5.Load(new System.IO.StreamReader(new System.IO.MemoryStream(binData), Encoding.GetEncoding(932)));
                            }
                            break;
                        }
                        Thread.Sleep(100);
                    }
                }

                //タスクトレイの表示
                taskTray.Click += (sender, e) =>
                {
                    Show();
                    WindowState = Settings.Instance.LastWindowState;
                    Activate();
                };
                taskTray.IconUri = new Uri("pack://application:,,,/Resources/TaskIconBlue.ico");

                CommonManager.Instance.DB.ReloadReserveInfo();
                ReserveData item = new ReserveData();
                if (CommonManager.Instance.DB.GetNextReserve(ref item) == true)
                {
                    String timeView = item.StartTime.ToString("M/d(ddd) H:mm～");
                    DateTime endTime = item.StartTime + TimeSpan.FromSeconds(item.DurationSecond);
                    timeView += endTime.ToString("H:mm");
                    taskTray.Text = "次の予約：" + item.StationName + " " + timeView + " " + item.Title;
                }
                else
                {
                    taskTray.Text = "次の予約なし";
                }

                ResetTaskMenu();

                CheckCmdLine();
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message + "\r\n" + ex.StackTrace);
            }
        }

        private void CheckCmdLine()
        {
            foreach (string arg in Environment.GetCommandLineArgs())
            {
                String ext = System.IO.Path.GetExtension(arg);
                if (string.Compare(ext, ".exe", true) == 0)
                {
                    //何もしない
                }
                else if (string.Compare(ext, ".tvpid", true) == 0 || string.Compare(ext, ".tvpio", true) == 0)
                {
                    //iEPG追加
                    IEPGFileClass iepgFile = new IEPGFileClass();
                    if (iepgFile.LoadTVPIDFile(arg) == true)
                    {
                        List<ReserveData> val = new List<ReserveData>();
                        val.Add(iepgFile.AddInfo);
                        CommonManager.CreateSrvCtrl().SendAddReserve(val);
                    }
                    else
                    {
                        MessageBox.Show("解析に失敗しました。デジタル用Version 2のiEPGの必要があります。");
                    }
                }
                else if (string.Compare(ext, ".tvpi", true) == 0)
                {
                    //iEPG追加
                    IEPGFileClass iepgFile = new IEPGFileClass();
                    if (iepgFile.LoadTVPIFile(arg) == true)
                    {
                        List<ReserveData> val = new List<ReserveData>();
                        val.Add(iepgFile.AddInfo);
                        CommonManager.CreateSrvCtrl().SendAddReserve(val);
                    }
                    else
                    {
                        MessageBox.Show("解析に失敗しました。放送局名がサービスに関連づけされていない可能性があります。");
                    }
                }
            }
        }

        private void ResetTaskMenu()
        {
            var addList = new List<KeyValuePair<string, Action<string>>>();
            foreach (string info in Settings.Instance.TaskMenuList)
            {
                if (info == "（セパレータ）")
                {
                    addList.Add(new KeyValuePair<string, Action<string>>(null, null));
                }
                else
                {
                    addList.Add(new KeyValuePair<string, Action<string>>(info, tag =>
                    {
                        if (tag == "設定")
                        {
                            //メインウィンドウが表示済みであることを前提にしている箇所があるため
                            if (PresentationSource.FromVisual(this) == null)
                            {
                                Show();
                                WindowState = Settings.Instance.LastWindowState;
                            }
                            SettingCmd();
                        }
                        else if (tag == "終了")
                        {
                            CloseCmd();
                        }
                        else if (tag == "スタンバイ")
                        {
                            StandbyCmd();
                        }
                        else if (tag == "休止")
                        {
                            SuspendCmd();
                        }
                        else if (tag == "EPG取得")
                        {
                            EpgCapCmd();
                        }
                        else if (tag == "再接続")
                        {
                            if (CommonManager.Instance.NWMode)
                            {
                                ConnectCmd(true);
                            }
                        }
                    }));
                }
            }
            taskTray.ContextMenuList = addList;
            taskTray.ForceHideBalloonTipSec = Settings.Instance.ForceHideBalloonTipSec;
            taskTray.Visible = Settings.Instance.ShowTray;
        }

        private void ResetButtonView()
        {
            stackPanel_button.Children.Clear();
            for (int i = 0; i < tabControl_main.Items.Count; i++)
            {
                TabItem ti = tabControl_main.Items.GetItemAt(i) as TabItem;
                if (ti != null && ti.Tag is string && ((string)ti.Tag).StartsWith("PushLike"))
                {
                    tabControl_main.Items.Remove(ti);
                    i--;
                }
            }
            foreach (string info in Settings.Instance.ViewButtonList)
            {
                if (String.Compare(info, "（空白）") == 0)
                {
                    Label space = new Label();
                    space.Width = 15;
                    stackPanel_button.Children.Add(space);
                }
                else
                {
                    if (buttonList.ContainsKey(info) == true)
                    {
                        if (String.Compare(info, "カスタム１") == 0)
                        {
                            buttonList[info].Content = Settings.Instance.Cust1BtnName;
                        }
                        if (String.Compare(info, "カスタム２") == 0)
                        {
                            buttonList[info].Content = Settings.Instance.Cust2BtnName;
                        }
                        stackPanel_button.Children.Add(buttonList[info]);

                        if (Settings.Instance.ViewButtonShowAsTab)
                        {
                            //ボタン風のタブを追加する
                            TabItem ti = new TabItem();
                            ti.Header = buttonList[info].Content;
                            ti.Tag = "PushLike" + info;
                            ti.Background = null;
                            ti.BorderBrush = null;
                            //タブ移動をキャンセルしつつ擬似的に対応するボタンを押す
                            ti.PreviewMouseDown += (sender, e) =>
                            {
                                if (e.ChangedButton == MouseButton.Left)
                                {
                                    buttonList[((string)((TabItem)sender).Tag).Substring(8)].RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                                    e.Handled = true;
                                }
                            };
                            tabControl_main.Items.Add(ti);
                        }
                    }
                }
            }
            //タブとして表示するかボタンが1つもないときは行を隠す
            rowDefinition_row0.Height = new GridLength(Settings.Instance.ViewButtonShowAsTab || stackPanel_button.Children.Count == 0 ? 0 : 30);
        }

        bool ConnectCmd(bool showDialog)
        {
            if (showDialog == true)
            {
                ConnectWindow dlg = new ConnectWindow();
                PresentationSource topWindow = PresentationSource.FromVisual(this);
                if (topWindow != null)
                {
                    dlg.Owner = (Window)topWindow.RootVisual;
                }
                if (dlg.ShowDialog() == false)
                {
                    return true;
                }
            }

            bool connected = false;
            try
            {
                //IPv4の名前解決を優先する
                foreach (var address in System.Net.Dns.GetHostAddresses(Settings.Instance.NWServerIP).OrderBy(a => a.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork))
                {
                    //コールバックは別スレッドかもしれないので設定は予めキャプチャする
                    uint execBat = Settings.Instance.ExecBat;
                    if (CommonManager.Instance.NW.ConnectServer(address, Settings.Instance.NWServerPort, Settings.Instance.NWWaitPort, (c, r) => OutsideCmdCallback(c, r, true, execBat)))
                    {
                        connected = true;
                        break;
                    }
                }
            }
            catch
            {
            }
            if (connected == false)
            {
                if (showDialog == true)
                {
                    MessageBox.Show("サーバーへの接続に失敗しました");
                }
                return false;
            }

            byte[] binData;
            if (CommonManager.CreateSrvCtrl().SendFileCopy("ChSet5.txt", out binData) == ErrCode.CMD_SUCCESS)
            {
                ChSet5.Load(new System.IO.StreamReader(new System.IO.MemoryStream(binData), Encoding.GetEncoding(932)));
            }
            CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.ReserveInfo);
            CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.RecInfo);
            CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.AutoAddEpgInfo);
            CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.AutoAddManualInfo);
            CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.EpgData);
            CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.PlugInFile);
            reserveView.UpdateReserveData();
            epgView.UpdateReserveData();
            tunerReserveView.UpdateReserveData();
            autoAddView.UpdateAutoAddInfo();
            recInfoView.UpdateInfo();
            epgView.UpdateEpgData();
            return true;
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            if (CommonManager.Instance.NWMode == true)
            {
                if (Settings.Instance.WakeReconnectNW == false || ConnectCmd(false) == false)
                {
                    ConnectCmd(true);
                }
            }
        }

        private void Window_Closing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            if (Settings.Instance.CloseMin == true && closeFlag == false)
            {
                e.Cancel = true;
                WindowState = System.Windows.WindowState.Minimized;
            }
            else
            {
                if (CommonManager.Instance.NWMode == false)
                {
                    if (initExe == true)
                    {
                        reserveView.SaveSize();
                        recInfoView.SaveSize();
                        autoAddView.SaveSize();

                        var cmd = CommonManager.CreateSrvCtrl();
                        cmd.SetConnectTimeOut(3000);
                        cmd.SendUnRegistGUI((uint)System.Diagnostics.Process.GetCurrentProcess().Id);
                        Settings.SaveToXmlFile();
                    }
                    pipeServer.StopServer();

                    if (mutex != null)
                    {
                        if (System.ServiceProcess.ServiceController.GetServices().All(sc => string.Compare(sc.ServiceName, "EpgTimer Service", true) != 0) && initExe == true)
                        {
                            var cmd = CommonManager.CreateSrvCtrl();
                            cmd.SetConnectTimeOut(3000);
                            cmd.SendClose();
                        }
                        mutex.ReleaseMutex();
                        mutex.Close();
                    }
                }
                else
                {
                    reserveView.SaveSize();
                    recInfoView.SaveSize();
                    autoAddView.SaveSize();

                    if (CommonManager.Instance.NW.IsConnected == true && needUnRegist == true)
                    {
                        CommonManager.CreateSrvCtrl().SendUnRegistTCP(Settings.Instance.NWWaitPort);
                    }
                    Settings.SaveToXmlFile();

                    if (mutex != null)
                    {
                        mutex.ReleaseMutex();
                        mutex.Close();
                    }
                }
                taskTray.Dispose();
            }
        }

        private void Window_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (this.WindowState == WindowState.Normal)
            {
                if (this.Visibility == System.Windows.Visibility.Visible && this.Width > 0 && this.Height > 0)
                {
                    Settings.Instance.MainWndWidth = this.Width;
                    Settings.Instance.MainWndHeight = this.Height;
                }
            }
        }

        private void Window_LocationChanged(object sender, EventArgs e)
        {
            if (this.WindowState == WindowState.Normal)
            {
                if (this.Visibility == System.Windows.Visibility.Visible && this.Top > 0 && this.Left > 0)
                {
                    Settings.Instance.MainWndTop = this.Top;
                    Settings.Instance.MainWndLeft = this.Left;
                }
            }
        }

        private void Window_StateChanged(object sender, EventArgs e)
        {
            if (this.WindowState == WindowState.Minimized)
            {
                if (Settings.Instance.ShowTray && Settings.Instance.MinHide)
                {
                    this.Visibility = System.Windows.Visibility.Hidden;
                }
            }
            if (this.WindowState == WindowState.Normal || this.WindowState == WindowState.Maximized)
            {
                this.Visibility = System.Windows.Visibility.Visible;
                Settings.Instance.LastWindowState = this.WindowState;
            }
        }

        private void Window_PreviewDragEnter(object sender, DragEventArgs e)
        {
            e.Handled = true;
        }

        private void Window_PreviewDrop(object sender, DragEventArgs e)
        {
            string[] filePath = e.Data.GetData(DataFormats.FileDrop, true) as string[];
            foreach (string path in filePath)
            {
                String ext = System.IO.Path.GetExtension(path);
                if (string.Compare(ext, ".tvpid", true) == 0 || string.Compare(ext, ".tvpio", true) == 0)
                {
                    //iEPG追加
                    IEPGFileClass iepgFile = new IEPGFileClass();
                    if (iepgFile.LoadTVPIDFile(path) == true)
                    {
                        List<ReserveData> val = new List<ReserveData>();
                        val.Add(iepgFile.AddInfo);
                        CommonManager.CreateSrvCtrl().SendAddReserve(val);
                    }
                    else
                    {
                        MessageBox.Show("解析に失敗しました。デジタル用Version 2のiEPGの必要があります。");
                    }
                }
                else if (string.Compare(ext, ".tvpi", true) == 0)
                {
                    //iEPG追加
                    IEPGFileClass iepgFile = new IEPGFileClass();
                    if (iepgFile.LoadTVPIFile(path) == true)
                    {
                        List<ReserveData> val = new List<ReserveData>();
                        val.Add(iepgFile.AddInfo);
                        CommonManager.CreateSrvCtrl().SendAddReserve(val);
                    }
                    else
                    {
                        MessageBox.Show("解析に失敗しました。放送局名がサービスに関連づけされていない可能性があります。");
                    }
                }
            }
        }

        private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            if (Keyboard.Modifiers == ModifierKeys.Control)
            {
                switch (e.Key)
                {
                    case Key.F:
                        if (e.IsRepeat == false)
                        {
                            this.buttonList["検索"].RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                        }
                        e.Handled = true;
                        break;
                    case Key.D1:
                        if (e.IsRepeat == false)
                        {
                            this.tabItem_reserve.IsSelected = true;
                        }
                        e.Handled = true;
                        break;
                    case Key.D2:
                        if (e.IsRepeat == false)
                        {
                            this.tabItem_tunerReserve.IsSelected = true;
                        }
                        e.Handled = true;
                        break;
                    case Key.D3:
                        if (e.IsRepeat == false)
                        {
                            this.tabItem_recinfo.IsSelected = true;
                        }
                        e.Handled = true;
                        break;
                    case Key.D4:
                        if (e.IsRepeat == false)
                        {
                            this.tabItem_epgAutoAdd.IsSelected = true;
                        }
                        e.Handled = true;
                        break;
                    case Key.D5:
                        if (e.IsRepeat == false)
                        {
                            this.tabItem_epg.IsSelected = true;
                        }
                        e.Handled = true;
                        break;
                }
            }
        }

        void settingButton_Click(object sender, RoutedEventArgs e)
        {
            SettingCmd();
        }

        void SettingCmd()
        {
            SettingWindow setting = new SettingWindow();
            PresentationSource topWindow = PresentationSource.FromVisual(this);
            if (topWindow != null)
            {
                setting.Owner = (Window)topWindow.RootVisual;
            }
            if (setting.ShowDialog() == true)
            {
                {
                    if (CommonManager.Instance.NWMode == true)
                    {
                        CommonManager.Instance.DB.SetNoAutoReloadEPG(Settings.Instance.NgAutoEpgLoadNW);
                    }
                    epgView.UpdateSetting();
                    ResetButtonView();
                    ResetTaskMenu();
                }
            }
        }

        void searchButton_Click(object sender, RoutedEventArgs e)
        {
            // Hide()したSearchWindowを復帰
            foreach (Window win1 in this.OwnedWindows)
            {
                if (win1.GetType() == typeof(SearchWindow))
                {
                    win1.Show();
                    return;
                }
            }
            //
            SearchCmd();
        }

        void SearchCmd()
        {
            SearchWindow search = new SearchWindow();
            PresentationSource topWindow = PresentationSource.FromVisual(this);
            if (topWindow != null)
            {
                search.Owner = (Window)topWindow.RootVisual;
            }
            search.SetViewMode(0);
            search.ShowDialog();
        }

        void closeButton_Click(object sender, RoutedEventArgs e)
        {
            CloseCmd();
        }

        void CloseCmd()
        {
            closeFlag = true;
            Close();
        }

        void epgCapButton_Click(object sender, RoutedEventArgs e)
        {
            EpgCapCmd();
        }

        void EpgCapCmd()
        {
            if (CommonManager.CreateSrvCtrl().SendEpgCapNow() != ErrCode.CMD_SUCCESS)
            {
                MessageBox.Show("EPG取得を行える状態ではありません。\r\n（もうすぐ予約が始まる。EPGデータ読み込み中。など）");
            }
        }

        void epgReloadButton_Click(object sender, RoutedEventArgs e)
        {
            EpgReloadCmd();
        }

        void EpgReloadCmd()
        {
            if (CommonManager.Instance.NWMode == true)
            {
                CommonManager.Instance.DB.SetOneTimeReloadEpg();
            }
            if (CommonManager.CreateSrvCtrl().SendReloadEpg() != ErrCode.CMD_SUCCESS)
            {
                MessageBox.Show("EPG再読み込みを行える状態ではありません。\r\n（EPGデータ読み込み中。など）");
            }
        }

        void suspendButton_Click(object sender, RoutedEventArgs e)
        {
            SuspendCmd();
        }

        void SuspendCmd()
        {
            ErrCode err = CommonManager.CreateSrvCtrl().SendChkSuspend();
            if (err != ErrCode.CMD_SUCCESS)
            {
                MessageBox.Show(CommonManager.GetErrCodeText(err) ?? "休止に移行できる状態ではありません。\r\n（もうすぐ予約が始まる。または抑制条件のexeが起動している。など）");
            }
            else
            {
                if (CommonManager.Instance.NWMode == false)
                {
                    if (Settings.Instance.SuspendChk == 1)
                    {
                        SuspendCheckWindow dlg = new SuspendCheckWindow();
                        dlg.SetMode(0, 2);
                        if (dlg.ShowDialog() == true)
                        {
                            return;
                        }
                    }
                    CommonManager.CreateSrvCtrl().SendSuspend(0xFF02);
                }
                else
                {
                    if (Settings.Instance.SuspendCloseNW == true)
                    {
                        if (CommonManager.Instance.NW.IsConnected == true)
                        {
                            CommonManager.CreateSrvCtrl().SendUnRegistTCP(Settings.Instance.NWWaitPort);
                            CommonManager.CreateSrvCtrl().SendSuspend(0xFF02);
                            closeFlag = true;
                            needUnRegist = false;
                            Close();
                        }
                    }
                    else
                    {
                        CommonManager.CreateSrvCtrl().SendSuspend(0xFF02);
                    }
                }
            }
        }

        void standbyButton_Click(object sender, RoutedEventArgs e)
        {
            StandbyCmd();
        }

        void StandbyCmd()
        {
            ErrCode err = CommonManager.CreateSrvCtrl().SendChkSuspend();
            if (err != ErrCode.CMD_SUCCESS)
            {
                MessageBox.Show(CommonManager.GetErrCodeText(err) ?? "スタンバイに移行できる状態ではありません。\r\n（もうすぐ予約が始まる。または抑制条件のexeが起動している。など）");
            }
            else
            {
                if (CommonManager.Instance.NWMode == false)
                {
                    if (Settings.Instance.SuspendChk == 1)
                    {
                        SuspendCheckWindow dlg = new SuspendCheckWindow();
                        dlg.SetMode(0, 1);
                        if (dlg.ShowDialog() == true)
                        {
                            return;
                        }
                    }
                    CommonManager.CreateSrvCtrl().SendSuspend(0xFF01);
                }
                else
                {
                    if (Settings.Instance.SuspendCloseNW == true)
                    {
                        if (CommonManager.Instance.NW.IsConnected == true)
                        {
                            CommonManager.CreateSrvCtrl().SendUnRegistTCP(Settings.Instance.NWWaitPort);
                            CommonManager.CreateSrvCtrl().SendSuspend(0xFF01);
                            closeFlag = true;
                            needUnRegist = false;
                            Close();
                        }
                    }
                    else
                    {
                        CommonManager.CreateSrvCtrl().SendSuspend(0xFF01);
                    }
                }
            }
        }

        void custum1Button_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                System.Diagnostics.Process.Start(Settings.Instance.Cust1BtnCmd, Settings.Instance.Cust1BtnCmdOpt);
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message);
            }
        }

        void custum2Button_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                System.Diagnostics.Process.Start(Settings.Instance.Cust2BtnCmd, Settings.Instance.Cust2BtnCmdOpt);
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message);
            }
        }

        void nwTVEndButton_Click(object sender, RoutedEventArgs e)
        {
            CommonManager.Instance.TVTestCtrl.CloseTVTest();
        }

        void logViewButton_Click(object sender, RoutedEventArgs e)
        {
            NotifyLogWindow dlg = new NotifyLogWindow();
            PresentationSource topWindow = PresentationSource.FromVisual(this);
            if (topWindow != null)
            {
                dlg.Owner = (Window)topWindow.RootVisual;
            }
            dlg.ShowDialog();
        }

        void connectButton_Click(object sender, RoutedEventArgs e)
        {
            if (CommonManager.Instance.NWMode == true)
            {
                ConnectCmd(true);
            }
        }

        private Tuple<ErrCode, byte[], uint> OutsideCmdCallback(uint cmdParam, byte[] cmdData, bool networkFlag, uint execBat)
        {
            System.Diagnostics.Trace.WriteLine((CtrlCmd)cmdParam);
            var res = new Tuple<ErrCode, byte[], uint>(ErrCode.CMD_NON_SUPPORT, null, 0);

            switch ((CtrlCmd)cmdParam)
            {
                case CtrlCmd.CMD_TIMER_GUI_SHOW_DLG:
                    if (networkFlag == false)
                    {
                        res = new Tuple<ErrCode, byte[], uint>(ErrCode.CMD_SUCCESS, null, 0);
                        Dispatcher.BeginInvoke(new Action(() => Visibility = Visibility.Visible));
                    }
                    break;
                case CtrlCmd.CMD_TIMER_GUI_VIEW_EXECUTE:
                    if (networkFlag == false)
                    {
                        //原作では成否にかかわらずCMD_SUCCESSだったが、サーバ側の仕様と若干矛盾するので変更した
                        res = new Tuple<ErrCode, byte[], uint>(ErrCode.CMD_ERR, null, 0);
                        String exeCmd = "";
                        (new CtrlCmdReader(new System.IO.MemoryStream(cmdData, false))).Read(ref exeCmd);
                        if (exeCmd.Length > 0 && exeCmd[0] == '"')
                        {
                            //形式は("FileName")か("FileName" Arguments..)のどちらか。ほかは拒否してよい
                            int i = exeCmd.IndexOf('"', 1);
                            if (i >= 2 && (exeCmd.Length == i + 1 || exeCmd[i + 1] == ' '))
                            {
                                var startInfo = new System.Diagnostics.ProcessStartInfo(exeCmd.Substring(1, i - 1));
                                if (exeCmd.Length > i + 2)
                                {
                                    startInfo.Arguments = exeCmd.Substring(i + 2);
                                }
                                if (startInfo.FileName.EndsWith(".bat", StringComparison.OrdinalIgnoreCase))
                                {
                                    if (execBat == 0)
                                    {
                                        startInfo.WindowStyle = System.Diagnostics.ProcessWindowStyle.Minimized;
                                    }
                                    else if (execBat == 1)
                                    {
                                        startInfo.WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden;
                                    }
                                }
                                //FileNameは実行ファイルか.batのフルパス。チェックはしない(安全性云々はここで考えることではない)
                                try
                                {
                                    //ShellExecute相当なので.batなどもそのまま与える
                                    var process = System.Diagnostics.Process.Start(startInfo);
                                    if (process != null)
                                    {
                                        var w = new CtrlCmdWriter(new System.IO.MemoryStream());
                                        w.Write(process.Id);
                                        w.Stream.Close();
                                        res = new Tuple<ErrCode, byte[], uint>(ErrCode.CMD_SUCCESS, w.Stream.ToArray(), 0);
                                    }
                                }
                                catch { }
                            }
                        }
                    }
                    break;
                case CtrlCmd.CMD_TIMER_GUI_QUERY_SUSPEND:
                    if (networkFlag == false)
                    {
                        res = new Tuple<ErrCode, byte[], uint>(ErrCode.CMD_SUCCESS, null, 0);

                        UInt16 param = 0;
                        (new CtrlCmdReader(new System.IO.MemoryStream(cmdData, false))).Read(ref param);

                        Dispatcher.BeginInvoke(new Action(() => ShowSleepDialog(param)));
                    }
                    break;
                case CtrlCmd.CMD_TIMER_GUI_QUERY_REBOOT:
                    if (networkFlag == false)
                    {
                        res = new Tuple<ErrCode, byte[], uint>(ErrCode.CMD_SUCCESS, null, 0);

                        UInt16 param = 0;
                        (new CtrlCmdReader(new System.IO.MemoryStream(cmdData, false))).Read(ref param);

                        Byte reboot = (Byte)((param & 0xFF00) >> 8);
                        Byte suspendMode = (Byte)(param & 0x00FF);

                        Dispatcher.BeginInvoke(new Action(() =>
                        {
                            SuspendCheckWindow dlg = new SuspendCheckWindow();
                            dlg.SetMode(reboot, suspendMode);
                            if (dlg.ShowDialog() != true)
                            {
                                CommonManager.CreateSrvCtrl().SendReboot();
                            }
                        }));
                    }
                    break;
                case CtrlCmd.CMD_TIMER_GUI_SRV_STATUS_NOTIFY2:
                    {
                        NotifySrvInfo status = new NotifySrvInfo();
                        var r = new CtrlCmdReader(new System.IO.MemoryStream(cmdData, false));
                        ushort version = 0;
                        r.Read(ref version);
                        r.Version = version;
                        r.Read(ref status);
                        //通知の巡回カウンタをItem3で返す
                        res = new Tuple<ErrCode, byte[], uint>(ErrCode.CMD_SUCCESS, null, status.param3);
                        Dispatcher.BeginInvoke(new Action(() => NotifyStatus(status)));
                    }
                    break;
            }
            return res;
        }

        private void ShowSleepDialog(UInt16 param)
        {
            if (IniFileHandler.GetPrivateProfileInt("NO_SUSPEND", "NoUsePC", 0, SettingPath.TimerSrvIniPath) == 1)
            {
                int ngUsePCTime = IniFileHandler.GetPrivateProfileInt("NO_SUSPEND", "NoUsePCTime", 3, SettingPath.TimerSrvIniPath);

                if (ngUsePCTime == 0 || CommonUtil.GetIdleTimeSec() < ngUsePCTime * 60)
                {
                    return;
                }
            }

            Byte suspendMode = (Byte)(param & 0x00FF);

            {
                SuspendCheckWindow dlg = new SuspendCheckWindow();
                dlg.SetMode(0, suspendMode);
                if (dlg.ShowDialog() != true)
                {
                    CommonManager.CreateSrvCtrl().SendSuspend(param);
                }
            }
        }

        void NotifyStatus(NotifySrvInfo status)
        {
            System.Diagnostics.Trace.WriteLine((UpdateNotifyItem)status.notifyID);

            switch ((UpdateNotifyItem)status.notifyID)
            {
                case UpdateNotifyItem.EpgData:
                    {
                        CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.EpgData);
                        if (CommonManager.Instance.NWMode == false)
                        {
                            CommonManager.Instance.DB.ReloadEpgData();
                        }
                        if (PresentationSource.FromVisual(Application.Current.MainWindow) != null)
                        {
                            epgView.UpdateEpgData();
                        }
                        GC.Collect();
                    }
                    break;
                case UpdateNotifyItem.ReserveInfo:
                    {
                        CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.ReserveInfo);
                        if (CommonManager.Instance.NWMode == false)
                        {
                            CommonManager.Instance.DB.ReloadReserveInfo();
                        }
                        reserveView.UpdateReserveData();
                        epgView.UpdateReserveData();
                        tunerReserveView.UpdateReserveData();
                    }
                    break;
                case UpdateNotifyItem.RecInfo:
                    {
                        CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.RecInfo);
                        if (CommonManager.Instance.NWMode == false)
                        {
                            CommonManager.Instance.DB.ReloadrecFileInfo();
                        }
                        recInfoView.UpdateInfo();
                    }
                    break;
                case UpdateNotifyItem.AutoAddEpgInfo:
                    {
                        CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.AutoAddEpgInfo);
                        if (CommonManager.Instance.NWMode == false)
                        {
                            CommonManager.Instance.DB.ReloadEpgAutoAddInfo();
                        }
                        autoAddView.UpdateAutoAddInfo();
                    }
                    break;
                case UpdateNotifyItem.AutoAddManualInfo:
                    {
                        CommonManager.Instance.DB.SetUpdateNotify((UInt32)UpdateNotifyItem.AutoAddManualInfo);
                        if (CommonManager.Instance.NWMode == false)
                        {
                            CommonManager.Instance.DB.ReloadManualAutoAddInfo();
                        }
                        autoAddView.UpdateAutoAddInfo();
                    }
                    break;
                case UpdateNotifyItem.SrvStatus:
                    {
                        if (status.param1 == 1)
                        {
                            taskTray.IconUri = new Uri("pack://application:,,,/Resources/TaskIconRed.ico");
                        }
                        else if (status.param1 == 2)
                        {
                            taskTray.IconUri = new Uri("pack://application:,,,/Resources/TaskIconGreen.ico");
                        }
                        else
                        {
                            taskTray.IconUri = new Uri("pack://application:,,,/Resources/TaskIconBlue.ico");
                        }
                    }
                    break;
                case UpdateNotifyItem.PreRecStart:
                case UpdateNotifyItem.RecStart:
                case UpdateNotifyItem.RecEnd:
                case UpdateNotifyItem.RecTuijyu:
                case UpdateNotifyItem.ChgTuijyu:
                case UpdateNotifyItem.PreEpgCapStart:
                case UpdateNotifyItem.EpgCapStart:
                case UpdateNotifyItem.EpgCapEnd:
                    if (Settings.Instance.NoBallonTips == false)
                    {
                        UpdateNotifyItem notifyID = (UpdateNotifyItem)status.notifyID;
                        string title = notifyID == UpdateNotifyItem.PreRecStart ? "予約録画開始準備" :
                                       notifyID == UpdateNotifyItem.RecStart ? "録画開始" :
                                       notifyID == UpdateNotifyItem.RecEnd ? "録画終了" :
                                       notifyID == UpdateNotifyItem.RecTuijyu ? "追従発生" :
                                       notifyID == UpdateNotifyItem.ChgTuijyu ? "番組変更" : "EPG取得";
                        string tips = notifyID == UpdateNotifyItem.EpgCapStart ? "開始" :
                                      notifyID == UpdateNotifyItem.EpgCapEnd ? "終了" : status.param4;
                        taskTray.ShowBalloonTip(title, tips, 10 * 1000);
                    }
                    CommonManager.Instance.NotifyLogList.Add(status);
                    break;
                default:
                    break;
            }

            CommonManager.Instance.DB.ReloadReserveInfo();
            ReserveData item = new ReserveData();

            if (CommonManager.Instance.DB.GetNextReserve(ref item) == true)
            {
                String timeView = item.StartTime.ToString("M/d(ddd) H:mm～");
                DateTime endTime = item.StartTime + TimeSpan.FromSeconds(item.DurationSecond);
                timeView += endTime.ToString("H:mm");
                taskTray.Text = "次の予約：" + item.StationName + " " + timeView + " " + item.Title;
            }
            else
            {
                taskTray.Text = "次の予約なし";
            }
        }

        public void moveTo_tabItem_epg()
        {
            new BlackoutWindow(this).showWindow(this.tabItem_epg.Header.ToString());
            this.tabItem_epg.IsSelected = true;
        }

        public void EmphasizeSearchButton(bool emphasize)
        {
            Button button1 = buttonList["検索"];
            if (Settings.Instance.ViewButtonList.Contains("検索") == false)
            {
                if (emphasize)
                {
                    stackPanel_button.Children.Add(button1);
                }
                else
                {
                    stackPanel_button.Children.Remove(button1);
                }
            }

            //検索ボタンを点滅させる
            if (emphasize)
            {
                button1.Effect = new System.Windows.Media.Effects.DropShadowEffect();
                var animation = new System.Windows.Media.Animation.DoubleAnimation
                {
                    From = 1.0,
                    To = 0.7,
                    RepeatBehavior = System.Windows.Media.Animation.RepeatBehavior.Forever,
                    AutoReverse = true
                };
                button1.BeginAnimation(Button.OpacityProperty, animation);
            }
            else
            {
                button1.BeginAnimation(Button.OpacityProperty, null);
                button1.Opacity = 1;
                button1.Effect = null;
            }

            //もしあればタブとして表示のタブも点滅させる
            foreach (var item in tabControl_main.Items)
            {
                TabItem ti = item as TabItem;
                if (ti != null && ti.Tag is string && (string)ti.Tag == "PushLike検索")
                {
                    if (emphasize)
                    {
                        var animation = new System.Windows.Media.Animation.DoubleAnimation
                        {
                            From = 1.0,
                            To = 0.1,
                            RepeatBehavior = System.Windows.Media.Animation.RepeatBehavior.Forever,
                            AutoReverse = true
                        };
                        ti.BeginAnimation(TabItem.OpacityProperty, animation);
                    }
                    else
                    {
                        ti.BeginAnimation(TabItem.OpacityProperty, null);
                        ti.Opacity = 1;
                    }
                    break;
                }
            }
        }

    }
}
