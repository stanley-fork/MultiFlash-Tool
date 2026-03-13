using System;
using System.Windows.Forms;

namespace SakuraEDL
{
    internal static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            if (args.Length > 0 && ProcessCommandLine(args))
                return;

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            using (var splash = new SplashForm())
            {
                splash.ShowDialog();
            }

            Application.Run(new Form1());
        }

        private static bool ProcessCommandLine(string[] args)
        {
            if (args[0] == "--help" || args[0] == "-h")
            {
                ShowHelp();
                return true;
            }

            Console.WriteLine("Unknown command: " + args[0]);
            Console.WriteLine();
            ShowHelp();
            return true;
        }

        private static void ShowHelp()
        {
            Console.WriteLine("SakuraEDL");
            Console.WriteLine();
            Console.WriteLine("Usage:");
            Console.WriteLine("  SakuraEDL.exe          Launch GUI");
            Console.WriteLine("  SakuraEDL.exe --help   Show help");
        }
    }
}
