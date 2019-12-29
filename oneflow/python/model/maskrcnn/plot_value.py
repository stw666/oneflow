# pip3 install -U altair vega_datasets jupyterlab --user
import altair as alt
import pandas as pd
import numpy as np
import os
import glob


def plot_value(df):
    if df.empty:
        return
    legends = df["legend"].unique()
    poly_data = pd.DataFrame(
        {"iter": np.linspace(df["iter"].min(), df["iter"].max(), 1000)}
    )
    for legend in legends:
        poly_data[legend + "-fit"] = np.poly1d(
            np.polyfit(
                df[df["legend"] == legend]["iter"],
                df[df["legend"] == legend]["value"],
                3,
            )
        )(poly_data["iter"])

    base = alt.Chart(df).interactive()

    # chart = base.mark_line()
    chart = base.mark_circle()

    polynomial_fit = (
        alt.Chart(poly_data)
        .transform_fold(
            [legend + "-fit" for legend in legends], as_=["legend", "value"]
        )
        .mark_line()
    )
    chart += polynomial_fit
    chart = chart.encode(alt.X("iter:Q", scale=alt.Scale(zero=False))).encode(
        alt.Y("value:Q")
    )
    chart = chart.encode(color="legend:N")
    chart.display()
    # chart.save("{}.png".format("".join(legends)))


def plot_by_legend(df):
    legends = df["legend"].unique()
    for legend in legends:
        df[df["legend"] == legend]
        plot_value(df[df["legend"] == legend])


def plot_many_by_legend(df_dict):
    legend_set_unsored = []
    legend_set_sorted = [
        "loss_rpn_box_reg",
        "loss_objectness",
        "lr",
        "loss_box_reg",
        "total_pos_inds_elem_cnt",
        "loss_classifier",
        "loss_mask",
        "elapsed_time",
    ]
    for _, df in df_dict.items():
        for legend in list(df["legend"].unique()):
            if (
                legend not in legend_set_sorted
                and "note" in df
                and df[df["legend"] == legend]["note"].isnull().all()
            ):
                legend_set_unsored.append(legend)
            else:
                if legend not in legend_set_sorted:
                    print("skipping legend: {}".format(legend))

    limit, rate = get_limit_and_rate(list(df_dict.values()))
    limit, rate = 506, 1
    for legend in legend_set_sorted + legend_set_unsored:
        df_by_legend = pd.concat(
            [
                update_legend(df[df["legend"] == legend].copy(), k)
                for k, df in df_dict.items()
            ],
            axis=0,
            sort=False,
        )
        df_by_legend = subset_and_mod(df_by_legend, limit, rate)
        plot_value(df_by_legend)


def update_legend(df, prefix):
    if df["legend"].size > 0:
        df["legend"] = df.apply(
            lambda row: "{}-{}".format(prefix, row["legend"]), axis=1
        )
    return df


COLUMNS = [
    "loss_rpn_box_reg",
    "loss_objectness",
    "loss_box_reg",
    "loss_classifier",
    "loss_mask",
]


def make_loss_frame(hisogram, column_index, legend="undefined"):
    assert column_index < len(COLUMNS)
    ndarray = np.array(hisogram)[:, [column_index, len(COLUMNS)]]
    return pd.DataFrame(
        {"iter": ndarray[:, 1], "legend": legend, "value": ndarray[:, 0]}
    )


def make_loss_frame5(losses_hisogram, source):
    return pd.concat(
        [
            make_loss_frame(losses_hisogram, column_index, legend=column_name)
            for column_index, column_name in enumerate(COLUMNS)
        ],
        axis=0,
        ignore_index=True,
    )


def wildcard_at(path, index):
    result = glob.glob(path)
    assert len(result) > 0, "there is no files in {}".format(path)
    result.sort(key=os.path.getmtime)
    return result[index]


def get_df(path, wildcard, index=-1, post_process=None):
    if os.path.isdir(path):
        path = wildcard_at(os.path.join(path, wildcard), index)
    print(path)
    df = pd.read_csv(path)
    if callable(post_process):
        df = post_process(df)
    return df


def get_limit_and_rate(dfs):
    maxs = [df["iter"].max() for df in dfs] + [df.size for df in dfs]

    limit = min(maxs)
    rate = 1
    if limit * len(dfs) > 5000:
        rate = limit / (5000 // len(dfs)) + 1
    return limit, rate


def subset_and_mod(df, limit, take_every_n):
    df_limited = df[df["iter"] < limit]
    return df_limited[df_limited["iter"] % take_every_n == 0]


def post_process_flow(df):
    df.drop(["rank", "note"], axis=1)
    if "primary_lr" in df["legend"].unique():
        df["legend"].replace("primary_lr", "lr", inplace=True)
    df = df.groupby(["iter", "legend"], as_index=False).mean()
    return df


def post_process_torch(df):
    if df[df["value"].notnull()]["iter"].min() == 0:
        df["iter"] += 1
    return df


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(argument_default=argparse.SUPPRESS)
    parser.add_argument("-d", "--metrics_dir", type=str)
    parser.add_argument("-o", "--oneflow_metrics_path", type=str)
    parser.add_argument("-p", "--pytorch_metrics_path", type=str)
    args = parser.parse_args()

    if hasattr(args, "metrics_dir"):
        flow_metrics_path = args.metrics_dir
        torch_metrics_path = args.metrics_dir

    if hasattr(args, "oneflow_metrics_path"):
        flow_metrics_path = args.oneflow_metrics_path

    if hasattr(args, "pytorch_metrics_path"):
        torch_metrics_path = args.pytorch_metrics_path

    assert os.path.exists(flow_metrics_path), "{} not found".format(
        flow_metrics_path
    )
    assert os.path.exists(torch_metrics_path), "{} not found".format(
        torch_metrics_path
    )

    limit, rate = (520, 1)

    plot_many_by_legend(
        {
            # "flow1": get_df(
            #     os.path.join(
            #         args.metrics_dir,
            #         "loss-520-batch_size-8-gpu-4-image_dir-('val2017',)-2019-12-29--15-55-19.csv",
            #     ),
            #     -1,
            #     post_process_flow,
            # ),
            "flow": get_df(
                flow_metrics_path, "loss*.csv", -1, post_process_flow
            ),
            "torch": get_df(
                torch_metrics_path, "torch*.csv", -1, post_process_torch
            ),
            # "torch1": get_df(
            #     os.path.join(
            #         args.metrics_dir,
            #         "torch-520-batch_size-8-image_dir-coco_instances_val2017_subsample_8_repeated-2019-12-29--06-25-23.csv",
            #     ),
            #     -1,
            #     post_process_torch,
            # ),
        }
    )
    # plot_many_by_legend(
    #     {
    #         "flow1": get_df(
    #             flow_metrics_path, "loss*.csv", -1, post_process_flow
    #         ),
    #         "flow2": get_df(
    #             flow_metrics_path, "loss*.csv", -2, post_process_flow
    #         ),
    #     }
    # )
    # plot_many_by_legend(
    #     {
    #         "torch1": get_df(
    #             flow_metrics_path, "torch*.csv", -1, post_process_torch
    #         ),
    #         "torch2": get_df(
    #             flow_metrics_path, "torch*.csv", -2, post_process_torch
    #         ),
    #     }
    # )

    # plot_by_legend(flow_df)
