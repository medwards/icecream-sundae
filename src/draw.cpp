/*
 * Command line Icecream status monitor
 * Copyright (C) 2018 by Garmin Ltd. or its subsidiaries.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <algorithm>
#include <cstdint>
#include <string>
#include <map>
#include <memory>
#include <unordered_set>

#include <glib.h>
#include <glib-unix.h>
#include <ncurses.h>

#include "main.hpp"
#include "draw.hpp"

static void print_job_graph(std::map<uint32_t, Job> const &jobs, int max_jobs);

struct HostDisplayData {
    uint32_t id = 0;
    std::string name;
    int max_jobs = 0;
    int pending_jobs = 0;
    int active_jobs = 0;
    int total_in = 0;
    int total_out = 0;
    bool no_remote = false;
    std::map<uint32_t, Job> jobs;

    typedef std::map<uint32_t, HostDisplayData> Map;
    typedef std::vector<HostDisplayData> List;
};

class Attr {
    public:
        Attr(int a, bool on=true) : m_attr(a), m_on(false)
        {
            setOn(on);
        }

        ~Attr()
        {
            off();
        }

        bool getOn() const { return m_on; }
        int getAttr() const { return m_attr; }

        void setOn(bool on)
        {
            if (m_on != on) {
                if (on)
                    attron(m_attr);
                else
                    attroff(m_attr);
                m_on = on;
            }

        }

        void toggle()
        {
            setOn(!m_on);
        }

        void on()
        {
            setOn(true);
        }

        void off()
        {
            setOn(false);
        }

    private:
        int m_attr;
        bool m_on;
};

class Column {
    public:
        typedef bool (*Compare)(HostDisplayData const &, HostDisplayData const &);

        virtual ~Column() {}

        virtual size_t getWidth(HostDisplayData::Map const &host_data) const
        {
            size_t max_width = std::max(getHeader().size(), getMinWidth());

            for (auto const h : host_data) {
                std::string s = getOutputString(h.second);
                max_width = std::max(max_width, s.size());
            }
            return max_width;
        }

        virtual std::string getHeader() const = 0;

        virtual void output(int row, HostDisplayData const &data) const
        {
            move(row, m_column);
            addstr(getOutputString(data).c_str());
        }

        void setColumn(int col)
        {
            m_column = col;
        }

        int getColumn() const
        {
            return m_column;
        }

        virtual Compare get_compare() const = 0;

    protected:
        Column() {}

        virtual std::string getOutputString(HostDisplayData const &) const
        {
            return "";
        }

        virtual size_t getMinWidth() const
        {
            return 0;
        }

        int m_column;

};

class NameColumn: public Column {
    public:
        NameColumn() : Column() {}
        virtual ~NameColumn() {}

        virtual std::string getHeader() const override
        {
            return "NAME";
        }

        virtual void output(int row, HostDisplayData const &data) const override
        {
            move(row, m_column);
            {
                Attr attr(COLOR_PAIR(hosts[data.id].getColor()) | ( data.no_remote ? A_UNDERLINE : 0 ));
                addstr(getOutputString(data).c_str());
            }
        }

        virtual Compare get_compare() const override
        {
            return compare;
        }

    protected:
        virtual std::string getOutputString(HostDisplayData const &data) const override
        {
            std::ostringstream ss;
            ss << data.name;
            return ss.str();
        }

    private:
        static bool compare(HostDisplayData const &a, HostDisplayData const &b)
        {
            return a.name < b.name;
        }
};

class JobsColumn: public Column {
    public:
        JobsColumn() : Column() {}
        virtual ~JobsColumn() {}

        virtual size_t getWidth(HostDisplayData::Map const &host_data) const override
        {
            size_t max_width = getHeader().size();

            for (auto const h : host_data)
                max_width = std::max(max_width, static_cast<size_t>(h.second.max_jobs) + 2);

            return max_width;
        }

        virtual std::string getHeader() const override
        {
            return "JOBS";
        }

        virtual void output(int row, HostDisplayData const &data) const override
        {
            move(row, m_column);
            print_job_graph(data.jobs, data.max_jobs);
        }

        virtual Compare get_compare() const override
        {
            return compare;
        }

    private:
        static bool compare(HostDisplayData const &a, HostDisplayData const &b)
        {
            return a.jobs.size() < b.jobs.size();
        }
};

#define SIMPLE_COLUMN(_name, _header, _attr, _min_width) \
    class _name: public Column { \
        public: \
            _name() : Column() {} \
            virtual ~_name() {} \
            virtual std::string getHeader() const override { return _header; } \
            virtual Compare get_compare() const override { return compare; } \
        protected: \
            virtual std::string getOutputString(HostDisplayData const &data) const override \
            { \
                std::ostringstream ss; ss << data._attr; return ss.str(); \
            } \
            virtual size_t getMinWidth() const override { return _min_width; } \
        private: \
            static bool compare(HostDisplayData const &a, HostDisplayData const &b) { return a._attr < b._attr; } \
    }

SIMPLE_COLUMN(InJobsColumn, "IN", total_in, 5);
SIMPLE_COLUMN(OutJobsColumn, "OUT", total_out, 5);
SIMPLE_COLUMN(ActiveJobsColumn, "ACTIVE", active_jobs, 0);
SIMPLE_COLUMN(PendingJobsColumn, "PENDING", pending_jobs, 0);
SIMPLE_COLUMN(CurrentJobsColumn, "CUR", jobs.size(), 0);
SIMPLE_COLUMN(MaxJobsColumn, "MAX", max_jobs, 0);
SIMPLE_COLUMN(IDColumn, "ID", id, 0);

static std::vector<uint32_t> host_order;
static std::vector<std::unique_ptr<Column> > columns;
static guint idle_source = 0;
static int header_color;
static int expand_color;
static int highlight_color;
static uint32_t current_host = 0;
static size_t current_col = 0;
static bool sort_reversed = false;

static gboolean process_input(gint fd, GIOCondition condition, gpointer user_data)
{
    int c = getch();
    auto cur_host = hosts.find(current_host);

    if (cur_host == hosts.end())
        current_host = 0;
    else
        hosts.at(current_host).highlighted = false;

    switch(c) {
    case KEY_UP:
    case 'k':
        if (cur_host != hosts.end()) {
            if (cur_host->second.current_position > 0) {
                current_host = host_order[cur_host->second.current_position - 1];
            }
        } else {
            current_host = host_order[0];
        }
        break;

    case KEY_DOWN:
    case 'j':
        if (cur_host != hosts.end()) {
            if (cur_host->second.current_position < host_order.size() - 1) {
                current_host = host_order[cur_host->second.current_position + 1];
            }
        } else {
            current_host = host_order[0];
        }
        break;

    case KEY_LEFT:
    case 'h':
        if (current_col > 0)
            current_col--;
        break;

    case KEY_RIGHT:
    case 'l':
        if (current_col < columns.size() - 1)
            current_col++;
        break;

    case '\t':
        current_col = (current_col + 1) % columns.size();
        break;

    case ' ':
        if (cur_host != hosts.end())
            cur_host->second.expanded = !cur_host->second.expanded;
        break;

    case 'a':
        all_expanded = !all_expanded;
        for (auto &h : hosts)
            h.second.expanded = all_expanded;
        break;

    case 'r':
        sort_reversed = !sort_reversed;
        break;

    case 'q':
        g_main_loop_quit(main_loop);
        break;
    }

    if (current_host)
        hosts.at(current_host).highlighted = true;

    trigger_redraw();
    return TRUE;
}

static void print_job_graph(std::map<uint32_t, Job> const &jobs, int max_jobs)
{
    addch('[');

    int cnt = 0;

    for (auto const j : jobs) {
        if (!j.second.active)
            continue;

        int color = 0;
        auto const h = hosts.find(j.second.clientid);
        if (h != hosts.end())
            color = h->second.getColor();

        Attr c(COLOR_PAIR(color));
        addch(j.second.is_local ? '%' : '=');

        cnt++;
    }

    for (int i = cnt; i < max_jobs; ++i)
        addch(' ');

    addch(']');
}

static gboolean on_idle_draw(gpointer* user_data)
{
    do_redraw();
    idle_source = 0;
    return FALSE;
}

static gboolean on_winch_signal(gpointer user_data)
{
    trigger_redraw();
    return TRUE;
}

static int assign_color(int *id, int fg, int bg)
{
    int ident = *id;
    (*id)++;

    init_pair(ident, fg, bg);
    return ident;
}

static void do_render()
{
    int total_job_slots = 0;
    int avail_servers = 0;

    int screen_rows;
    int screen_cols;
    HostDisplayData::Map host_data;
    std::unordered_set<uint32_t> used_hosts;

    getmaxyx(stdscr, screen_rows, screen_cols);

    int pending_jobs = 0;
    int active_jobs = 0;
    int local_jobs = 0;

    for (auto const j : jobs) {
        if (j.second.active) {
            host_data[j.second.clientid].active_jobs++;
            active_jobs++;
        } else {
            host_data[j.second.clientid].pending_jobs++;
            pending_jobs++;
        }

        if (j.second.is_local)
            local_jobs++;

        if (j.second.hostid) {
            host_data[j.second.hostid].jobs[j.first] = j.second;
            used_hosts.insert(j.second.hostid);
        }
    }

    for (auto const h : hosts) {
        auto &data = host_data[h.first];

        data.id = h.first;
        data.name = h.second.getName();
        data.max_jobs = h.second.getMaxJobs();
        data.total_in = h.second.total_in;
        data.total_out = h.second.total_out;

        data.no_remote = h.second.getNoRemote();

        if (!h.second.getNoRemote()) {
            avail_servers++;
            total_job_slots += data.max_jobs;
        }
    }

    int row = 0;
    #define next_row() if (++row >= screen_rows) return

    move(row, 0);
    {
        Attr bold(A_BOLD);
        addstr("Scheduler: ");
    }
    addstr(current_scheduler_name.c_str());

    {
        Attr bold(A_BOLD);
        addstr(" Netname: ");
    }
    addstr(current_net_name.c_str());
    next_row();


    move(row, 0);
    {
        Attr bold(A_BOLD);
        addstr("Servers: ");
    }
    {
        std::ostringstream ss;
        ss << "Total:" << hosts.size() << " Available:" << avail_servers << " Active:" << used_hosts.size();
        addstr(ss.str().c_str());
    }
    next_row();

    move(row, 0);
    {
        Attr bold(A_BOLD);
        addstr("Jobs: ");
    }
    {
        std::ostringstream ss;
        ss << "Maxiumum:" << total_job_slots << " Active:" << active_jobs << " Local:" << local_jobs << " Pending:" << pending_jobs << " Total:" << total_jobs;
        addstr(ss.str().c_str());
    }
    next_row();

    move(row, 6);
    print_job_graph(jobs, total_job_slots);
    next_row();
    next_row();

    move(row, 0);
    {
        Attr color(COLOR_PAIR(header_color));
        Attr highlight(COLOR_PAIR(highlight_color), false);

        add_wch(sort_reversed ? WACS_UARROW : WACS_DARROW);
        for (int i = 1; i < screen_cols; i++)
            addch(' ');

        int col = 2;
        for (size_t i = 0; i < columns.size(); i++) {
            auto &c = columns[i];
            size_t width = c->getWidth(host_data);
            c->setColumn(col);

            if (current_col == i) {
                color.off();
                highlight.on();
            }

            mvprintw(row, col, "%-*s", width, c->getHeader().c_str());

            if (current_col == i) {
                highlight.off();
                color.on();
            }

            col += width + 1;
        }
    }
    next_row();

    HostDisplayData::List sorted_host_data;
    for (auto const &h : host_data)
        sorted_host_data.emplace_back(h.second);

    if (current_col < columns.size()) {
        auto compare = columns[current_col]->get_compare();
        if (sort_reversed)
            std::sort(sorted_host_data.rbegin(), sorted_host_data.rend(), compare);
        else
            std::sort(sorted_host_data.begin(), sorted_host_data.end(), compare);
    }

    host_order.clear();

    for (auto &data : sorted_host_data) {
        auto const id = data.id;
        auto &host = hosts[id];
        if (!data.id)
            continue;

        host.current_position = host_order.size();
        host_order.push_back(id);

        move(row, 0);
        {
            Attr color(COLOR_PAIR(host.highlighted ? highlight_color : expand_color));
            addch(hosts[id].expanded ? '-' : '+');
        }

        for (auto const &c: columns)
            c->output(row, data);

        if (hosts[id].expanded) {
            size_t width = 0;
            for (auto const &a : hosts[id].attr) {
                width = std::max(width, a.first.size());
            }

            for (auto const &a : hosts[id].attr) {
                next_row();
                move(row, 2);
                {
                    Attr bold(A_BOLD);
                    addstr(a.first.c_str());
                }
                move(row, 2 + width + 1);
                addstr(a.second.c_str());
            }
        }
        next_row();
    }
}

void do_redraw()
{
    clear();
    do_render();
    refresh();
}

void trigger_redraw()
{
    if (!idle_source)
        idle_source = g_idle_add(reinterpret_cast<GSourceFunc>(on_idle_draw), NULL);
}

CursesMode::CursesMode()
{
    initscr();

    cbreak();
    use_default_colors();
    start_color();
    curs_set(0);
    noecho();
    nonl();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    int color_id = 1;
    Host::addColor(assign_color(&color_id, COLOR_RED, -1));
    Host::addColor(assign_color(&color_id, COLOR_GREEN, -1));
    Host::addColor(assign_color(&color_id, COLOR_YELLOW, -1));
    Host::addColor(assign_color(&color_id, COLOR_BLUE, -1));
    Host::addColor(assign_color(&color_id, COLOR_MAGENTA, -1));
    Host::addColor(assign_color(&color_id, COLOR_CYAN, -1));
    Host::addColor(assign_color(&color_id, COLOR_WHITE, -1));

    header_color = assign_color(&color_id, COLOR_BLACK, COLOR_GREEN);
    expand_color = assign_color(&color_id, COLOR_GREEN, -1);
    highlight_color = assign_color(&color_id, COLOR_BLACK, COLOR_CYAN);

    g_unix_signal_add(SIGWINCH, reinterpret_cast<GSourceFunc>(on_winch_signal), nullptr);

    columns.emplace_back(std::make_unique<IDColumn>());
    columns.emplace_back(std::make_unique<NameColumn>());
    columns.emplace_back(std::make_unique<InJobsColumn>());
    columns.emplace_back(std::make_unique<CurrentJobsColumn>());
    columns.emplace_back(std::make_unique<MaxJobsColumn>());
    columns.emplace_back(std::make_unique<JobsColumn>());
    columns.emplace_back(std::make_unique<OutJobsColumn>());
    columns.emplace_back(std::make_unique<ActiveJobsColumn>());
    columns.emplace_back(std::make_unique<PendingJobsColumn>());

    g_unix_fd_add(STDIN_FILENO, G_IO_IN, process_input, main_loop);

    trigger_redraw();
}

CursesMode::~CursesMode()
{
    endwin();
}
