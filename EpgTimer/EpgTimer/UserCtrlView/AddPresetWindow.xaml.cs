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
using System.Windows.Shapes;

namespace EpgTimer
{
    /// <summary>
    /// AddPresetWindow.xaml の相互作用ロジック
    /// </summary>
    public partial class AddPresetWindow : Window
    {
        public AddPresetWindow()
        {
            InitializeComponent();
        }

        public void SetMode(bool chgMode)
        {
            if (chgMode == true)
            {
                button_add.Content = "変更";
                label_chgMsg.Visibility = System.Windows.Visibility.Visible;
            }
            else
            {
                button_add.Content = "追加";
            }
        }

        public string PresetName
        {
            get
            {
                return textBox_name.Text;
            }
            set
            {
                textBox_name.Text = value;
            }
        }

        private void button_add_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = true;
        }
    }
}
