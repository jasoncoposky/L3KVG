import { useState, useEffect } from 'react';
import ForceGraph2D from 'react-force-graph-2d';
import {
  ThemeProvider,
  createTheme,
  CssBaseline,
  Box,
  Card,
  CardContent,
  Typography,
  AppBar,
  Toolbar
} from '@mui/material';
import StorageIcon from '@mui/icons-material/Storage';
import '@fontsource/roboto/300.css';
import '@fontsource/roboto/400.css';
import '@fontsource/roboto/500.css';
import '@fontsource/roboto/700.css';
import MemoryIcon from '@mui/icons-material/Memory';
import SpeedIcon from '@mui/icons-material/Speed';

// Google Material Design 3 (Material You) Theme Defaults
const md3DarkTheme = createTheme({
  palette: {
    mode: 'dark',
    primary: {
      main: '#a8c7fa', // MD3 sys.color.primary dark
    },
    secondary: {
      main: '#9cc2a7', // sys.color.secondary dark
    },
    error: {
      main: '#ffb4ab', // sys.color.error dark
    },
    background: {
      default: '#1c1b1f', // sys.color.background dark
      paper: '#2b2930',   // sys.color.surface dark
    }
  },
  typography: {
    fontFamily: '"Roboto", "Helvetica", "Arial", sans-serif',
    h6: {
      fontWeight: 500,
      letterSpacing: '0.15px'
    },
    subtitle1: {
      fontWeight: 500,
      letterSpacing: '0.1px'
    },
    body2: {
      letterSpacing: '0.25px'
    }
  },
  shape: {
    borderRadius: 16 // MD3 standard elevated card radius
  },
  components: {
    MuiCard: {
      styleOverrides: {
        root: {
          backgroundImage: 'none', // Remove default MUI elevated overlay
          border: '1px solid rgba(255, 255, 255, 0.05)',
          boxShadow: '0px 4px 8px 3px rgba(0, 0, 0, 0.15), 0px 1px 3px rgba(0, 0, 0, 0.3)'
        }
      }
    }
  }
});

function App() {
  const [metrics, setMetrics] = useState({ hop_latency_us: 0, serialization_time_us: 0, cache_hits: 0, cache_misses: 0 });
  const [graphData, setGraphData] = useState<{ nodes: any[], links: any[] }>({ nodes: [], links: [] });

  const fetchMetrics = async () => {
    try {
      const res = await fetch('http://localhost:8080/api/metrics');
      const data = await res.json();
      setMetrics(data);
    } catch (e) {
      console.error(e);
    }
  };

  const runQuery = async () => {
    try {
      const res = await fetch('http://localhost:8080/api/query', {
        method: 'POST',
        body: "MATCH (n {id: 'npc_1', type: 'npc'})-[e:loyalty]->(ally) WHERE e.weight >= 0.0 RETURN ally.name"
      });
      const data = await res.json();

      const mappedNodes: any[] = [
        { id: "npc_1", name: "Aragorn (Root)", val: 6, color: md3DarkTheme.palette.primary.main }
      ];
      const mappedLinks: any[] = [];
      data.forEach((row: any, i: number) => {
        const allyName = row["ally.name"];
        const allyId = "ally_" + i;
        mappedNodes.push({ id: allyId, name: allyName, val: 3, color: md3DarkTheme.palette.secondary.main });
        mappedLinks.push({ source: "npc_1", target: allyId, width: 2 });
      });

      setGraphData({
        nodes: mappedNodes,
        links: mappedLinks
      });
    } catch (e) {
      console.error(e);
    }
  };

  useEffect(() => {
    fetchMetrics();
    runQuery();
    const interval = setInterval(fetchMetrics, 2000);
    return () => clearInterval(interval);
  }, []);

  return (
    <ThemeProvider theme={md3DarkTheme}>
      <CssBaseline />
      <Box sx={{ flexGrow: 1, height: '100vh', display: 'flex', flexDirection: 'column' }}>
        <AppBar position="static" elevation={0} sx={{ background: md3DarkTheme.palette.background.default, borderBottom: '1px solid rgba(255,255,255,0.08)' }}>
          <Toolbar>
            <StorageIcon sx={{ mr: 2, color: 'primary.main' }} />
            <Typography variant="h6" component="div" sx={{ flexGrow: 1 }}>
              L3KV Graph Engine : SRE Monitor
            </Typography>
          </Toolbar>
        </AppBar>

        <Box sx={{ p: 3, flexGrow: 1, overflow: 'auto', display: 'flex', flexDirection: 'column', gap: 3 }}>

          {/* Top Row: SRE Instrumentation */}
          <Box sx={{ display: 'flex', flexDirection: { xs: 'column', md: 'row' }, gap: 3 }}>
            {/* Hop Latency Card */}
            <Box sx={{ flex: 1 }}>
              <Card>
                <CardContent>
                  <Box sx={{ display: 'flex', alignItems: 'center', mb: 2 }}>
                    <SpeedIcon sx={{ color: metrics.hop_latency_us < 500 ? 'primary.main' : 'error.main', mr: 1 }} />
                    <Typography variant="subtitle1" color="text.secondary">
                      Traversal Hop Latency
                    </Typography>
                  </Box>
                  <Typography variant="h3" component="div" sx={{ mb: 1, color: metrics.hop_latency_us < 500 ? 'text.primary' : 'error.main' }}>
                    {metrics.hop_latency_us} <Typography variant="h6" component="span" color="text.secondary">μs</Typography>
                  </Typography>
                  <Box sx={{ width: '100%', height: 4, bgcolor: 'background.default', borderRadius: 2, overflow: 'hidden' }}>
                    <Box sx={{
                      width: `${Math.min((metrics.hop_latency_us / 1000) * 100, 100)}%`,
                      height: '100%',
                      bgcolor: metrics.hop_latency_us < 500 ? 'primary.main' : 'error.main'
                    }} />
                  </Box>
                </CardContent>
              </Card>
            </Box>

            {/* LRU Cache Efficacy Card */}
            <Box sx={{ flex: 1 }}>
              <Card>
                <CardContent>
                  <Box sx={{ display: 'flex', alignItems: 'center', mb: 2 }}>
                    <MemoryIcon sx={{ color: 'secondary.main', mr: 1 }} />
                    <Typography variant="subtitle1" color="text.secondary">
                      Resident Node LRU Hits
                    </Typography>
                  </Box>
                  <Typography variant="h3" component="div" sx={{ mb: 1 }}>
                    {metrics.cache_hits.toLocaleString()}
                  </Typography>
                  <Typography variant="body2" color="error.main">
                    {metrics.cache_misses.toLocaleString()} disk fetches (misses)
                  </Typography>
                </CardContent>
              </Card>
            </Box>

            {/* Protocol Info Card */}
            <Box sx={{ flex: 1 }}>
              <Card sx={{ bgcolor: 'rgba(168, 199, 250, 0.08)', height: '100%' }}>
                <CardContent>
                  <Typography variant="subtitle2" color="primary.main" sx={{ mb: 1 }}>
                    Active Protocol Pipeline
                  </Typography>
                  <Typography variant="body2" color="text.secondary" sx={{ display: 'flex', alignItems: 'center', gap: 1, mb: 1 }}>
                    • Zero-Copy Serialization Enabled
                  </Typography>
                  <Typography variant="body2" color="text.secondary" sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>
                    • Lexicographical Engine Prefetch
                  </Typography>
                </CardContent>
              </Card>
            </Box>
          </Box>

          {/* Bottom Row: Visualization Canvas */}
          <Card sx={{ flexGrow: 1, minHeight: 600, display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
            <ForceGraph2D
              graphData={graphData}
              width={1600}
              height={800}
              nodeLabel="name"
              nodeColor="color"
              nodeVal="val"
              linkColor={() => "rgba(255, 255, 255, 0.6)"}
              linkDirectionalArrowLength={6}
              linkDirectionalArrowRelPos={1}
              linkWidth="width"
              backgroundColor={md3DarkTheme.palette.background.paper}
            />
          </Card>

        </Box>
      </Box>
    </ThemeProvider>
  );
}

export default App;
